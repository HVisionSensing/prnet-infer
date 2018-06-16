/*
The MIT License (MIT)

Copyright (c) 2015 - 2017 Light Transport Entertainment, Inc.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#ifdef _MSC_VER
#pragma warning(disable : 4018)
#pragma warning(disable : 4244)
#pragma warning(disable : 4189)
#pragma warning(disable : 4996)
#pragma warning(disable : 4267)
#pragma warning(disable : 4477)
#endif

#include "render.h"

#include <atomic>
#include <chrono>  // C++11
#include <sstream>
#include <thread>  // C++11
#include <vector>

#include <iostream>

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-variable"
#endif

#include "matrix.h"
#include "nanort.h"

#include "trackball.h"

//#define STB_IMAGE_IMPLEMENTATION
//#include "stb_image.h"

#ifdef __clang__
#pragma clang diagnostic pop
#endif

#ifdef WIN32
#undef min
#undef max
#endif

namespace example {

// PCG32 code / (c) 2014 M.E. O'Neill / pcg-random.org
// Licensed under Apache License 2.0 (NO WARRANTY, etc. see website)
// http://www.pcg-random.org/
typedef struct {
  unsigned long long state;
  unsigned long long inc;  // not used?
} pcg32_state_t;

//#define PCG32_INITIALIZER \
//  { 0x853c49e6748fea9bULL, 0xda3e39cb94b95bdbULL }

static float pcg32_random(pcg32_state_t* rng) {
  unsigned long long oldstate = rng->state;
  rng->state = oldstate * 6364136223846793005ULL + rng->inc;
  unsigned int xorshifted = static_cast<unsigned int>(((oldstate >> 18u) ^ oldstate) >> 27u);
  unsigned int rot = oldstate >> 59u;
  unsigned int ret =
      (xorshifted >> rot) | (xorshifted << ((-static_cast<int>(rot)) & 31));

  return float(double(ret) / double(4294967296.0));
}

static void pcg32_srandom(pcg32_state_t* rng, uint64_t initstate, uint64_t initseq) {
  rng->state = 0U;
  rng->inc = (initseq << 1U) | 1U;
  pcg32_random(rng);
  rng->state += initstate;
  pcg32_random(rng);
}

const float kPI = 3.141592f;


#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
#pragma clang diagnostic ignored "-Wglobal-constructors"
#endif

static nanort::BVHAccel<float> gAccel;

#ifdef __clang__
#pragma clang diagnostic pop
#endif

typedef nanort::real3<float> float3;

inline float3 Lerp3(float3 v0, float3 v1, float3 v2, float u, float v) {
  return (1.0f - u - v) * v0 + u * v1 + v * v2;
}

inline void CalcNormal(float3& N, float3 v0, float3 v1, float3 v2) {
  float3 v10 = v1 - v0;
  float3 v20 = v2 - v0;

  N = vcross(v20, v10);
  N = vnormalize(N);
}

// Simple texture lookup without filtering.
static void FetchTexture(const prnet::Image<float>& image, float u, float v,
                  float* col) {
  if (image.getWidth() == 0) {
    return;
  }

  int tx = std::max(
      0, std::min(int(image.getWidth()) - 1, int(u * image.getWidth())));
  int ty = std::max(
      0, std::min(int(image.getHeight()) - 1, int(v * image.getHeight())));
  size_t idx_offset = (size_t(ty) * image.getWidth() + size_t(tx)) * image.getChannels();

  col[0] = image.getData()[idx_offset + 0];
  col[1] = image.getData()[idx_offset + 1];
  col[2] = image.getData()[idx_offset + 2];
}

static void BuildCameraFrame(float3* origin, float3* corner, float3* u, float3* v,
                      float quat[4], float eye[3], float lookat[3], float up[3],
                      float fov, int width, int height) {
  float e[4][4];

  Matrix::LookAt(e, eye, lookat, up);

  float r[4][4];
  build_rotmatrix(r, quat);

  float3 lo;
  lo[0] = lookat[0] - eye[0];
  lo[1] = lookat[1] - eye[1];
  lo[2] = lookat[2] - eye[2];
  float dist = vlength(lo);

  float dir[3];
  dir[0] = 0.0;
  dir[1] = 0.0;
  dir[2] = dist;

  Matrix::Inverse(r);

  float rr[4][4];
  float re[4][4];
  float zero[3] = {0.0f, 0.0f, 0.0f};
  float localUp[3] = {0.0f, 1.0f, 0.0f};
  Matrix::LookAt(re, dir, zero, localUp);

  // translate
  re[3][0] += eye[0];  // 0.0; //lo[0];
  re[3][1] += eye[1];  // 0.0; //lo[1];
  re[3][2] += (eye[2] - dist);

  // rot -> trans
  Matrix::Mult(rr, r, re);

  float m[4][4];
  for (int j = 0; j < 4; j++) {
    for (int i = 0; i < 4; i++) {
      m[j][i] = rr[j][i];
    }
  }

  float vzero[3] = {0.0f, 0.0f, 0.0f};
  float eye1[3];
  Matrix::MultV(eye1, m, vzero);

  float lookat1d[3];
  dir[2] = -dir[2];
  Matrix::MultV(lookat1d, m, dir);
  float3 lookat1(lookat1d[0], lookat1d[1], lookat1d[2]);

  float up1d[3];
  Matrix::MultV(up1d, m, up);

  float3 up1(up1d[0], up1d[1], up1d[2]);

  // absolute -> relative
  up1[0] -= eye1[0];
  up1[1] -= eye1[1];
  up1[2] -= eye1[2];
  // printf("up1(after) = %f, %f, %f\n", up1[0], up1[1], up1[2]);

  // Use original up vector
  // up1[0] = up[0];
  // up1[1] = up[1];
  // up1[2] = up[2];

  {
    float flen =
        (0.5f * float(height) / std::tan(0.5f * float(fov * kPI / 180.0f)));
    float3 look1;
    look1[0] = lookat1[0] - eye1[0];
    look1[1] = lookat1[1] - eye1[1];
    look1[2] = lookat1[2] - eye1[2];
    // vcross(u, up1, look1);
    // flip
    (*u) = nanort::vcross(look1, up1);
    (*u) = vnormalize((*u));

    (*v) = vcross(look1, (*u));
    (*v) = vnormalize((*v));

    look1 = vnormalize(look1);
    look1[0] = flen * look1[0] + eye1[0];
    look1[1] = flen * look1[1] + eye1[1];
    look1[2] = flen * look1[2] + eye1[2];
    (*corner)[0] = look1[0] - 0.5f * (width * (*u)[0] + height * (*v)[0]);
    (*corner)[1] = look1[1] - 0.5f * (width * (*u)[1] + height * (*v)[1]);
    (*corner)[2] = look1[2] - 0.5f * (width * (*u)[2] + height * (*v)[2]);

    (*origin)[0] = eye1[0];
    (*origin)[1] = eye1[1];
    (*origin)[2] = eye1[2];
  }
}

bool Renderer::BuildBVH() {
  std::cout << "[Build BVH] " << std::endl;

  nanort::BVHBuildOptions<float> build_options;  // Use default option
  build_options.cache_bbox = false;

  printf("  BVH build option:\n");
  printf("    # of leaf primitives: %d\n", build_options.min_leaf_primitives);
  printf("    SAH binsize         : %d\n", build_options.bin_size);

  auto t_start = std::chrono::system_clock::now();

  nanort::TriangleMesh<float> triangle_mesh(
      mesh_.vertices.data(), mesh_.faces.data(), sizeof(float) * 3);
  nanort::TriangleSAHPred<float> triangle_pred(
      mesh_.vertices.data(), mesh_.faces.data(), sizeof(float) * 3);

  printf("num_triangles = %lu\n", mesh_.faces.size() / 3);

  bool ret = gAccel.Build(uint32_t(mesh_.faces.size() / 3), triangle_mesh, triangle_pred,
                          build_options);
  (void) ret;
  assert(ret);

  auto t_end = std::chrono::system_clock::now();

  std::chrono::duration<double, std::milli> ms = t_end - t_start;
  std::cout << "BVH build time: " << ms.count() << " [ms]\n";

  nanort::BVHBuildStatistics stats = gAccel.GetStatistics();

  printf("  BVH statistics:\n");
  printf("    # of leaf   nodes: %d\n", stats.num_leaf_nodes);
  printf("    # of branch nodes: %d\n", stats.num_branch_nodes);
  printf("  Max tree depth     : %d\n", stats.max_tree_depth);
  float bmin[3], bmax[3];
  gAccel.BoundingBox(bmin, bmax);
  std::cout << "Bmin              : " << bmin[0] << ", " << bmin[1] << ", " << bmin[2] << std::endl;
  std::cout << "Bmax              : " << bmax[0] << ", " << bmax[1] << ", " << bmax[2] << std::endl;

  return true;
}

bool Renderer::Render(RenderBuffer* buffer, float quat[4],
                      const RenderConfig& config) {
  if (!gAccel.IsValid()) {
    return false;
  }

  int width = config.width;
  int height = config.height;

  // camera
  float eye[3] = {config.eye[0], config.eye[1], config.eye[2]};
  float look_at[3] = {config.look_at[0], config.look_at[1], config.look_at[2]};
  float up[3] = {config.up[0], config.up[1], config.up[2]};
  float fov = config.fov;
  float3 origin, corner, u, v;
  BuildCameraFrame(&origin, &corner, &u, &v, quat, eye, look_at, up, fov, width,
                   height);


  std::vector<std::thread> workers;
  std::atomic<int> i(0);

  uint32_t num_threads = std::max(1U, std::thread::hardware_concurrency());

  // Initialize RNG.

  for (uint64_t t = 0; t < uint64_t(num_threads); t++) {
    workers.emplace_back(std::thread([&, t]() {
      pcg32_state_t rng;
      pcg32_srandom(&rng, static_cast<unsigned long long>(config.pass),
                    t);  // seed = combination of render pass + thread no.

      int y = 0;
      while ((y = i++) < config.height) {

        for (int x = 0; x < config.width; x++) {
          nanort::Ray<float> ray;
          ray.org[0] = origin[0];
          ray.org[1] = origin[1];
          ray.org[2] = origin[2];

          float u0 = pcg32_random(&rng);
          float u1 = pcg32_random(&rng);

          float3 dir;
          // dir = corner + (float(x) + u0) * u +
          //      (float(config.height - y - 1) + u1) * v;
          dir = corner + (float(x) + u0) * u + (float(y) + u1) * v;
          dir = vnormalize(dir);
          ray.dir[0] = dir[0];
          ray.dir[1] = dir[1];
          ray.dir[2] = dir[2];

          float kFar = 1.0e+30f;
          ray.min_t = 0.0f;
          ray.max_t = kFar;

          size_t pidx = size_t(y * config.width + x);

          // clear pixel
          {
            buffer->rgba[4 * pidx + 0] = 0.0f;
            buffer->rgba[4 * pidx + 1] = 0.0f;
            buffer->rgba[4 * pidx + 2] = 0.0f;
            buffer->rgba[4 * pidx + 3] = 0.0f;

            buffer->normal[4 * pidx + 0] = 0.0f;
            buffer->normal[4 * pidx + 1] = 0.0f;
            buffer->normal[4 * pidx + 2] = 0.0f;
            buffer->normal[4 * pidx + 3] = 0.0f;
            buffer->position[4 * pidx + 0] = 0.0f;
            buffer->position[4 * pidx + 1] = 0.0f;
            buffer->position[4 * pidx + 2] = 0.0f;
            buffer->position[4 * pidx + 3] = 0.0f;
            buffer->depth[4 * pidx + 0] = 1000.0f;
            buffer->depth[4 * pidx + 1] = 1000.0f;
            buffer->depth[4 * pidx + 2] = 1000.0f;
            buffer->depth[4 * pidx + 3] = 1000.0f;
            buffer->texcoord[4 * pidx + 0] = 0.0f;
            buffer->texcoord[4 * pidx + 1] = 0.0f;
            buffer->texcoord[4 * pidx + 2] = 0.0f;
            buffer->texcoord[4 * pidx + 3] = 0.0f;
            buffer->diffuse[4 * pidx + 0] = 0.0f;
            buffer->diffuse[4 * pidx + 1] = 0.0f;
            buffer->diffuse[4 * pidx + 2] = 0.0f;
            buffer->diffuse[4 * pidx + 3] = 0.0f;
          }

          nanort::TriangleIntersector<> triangle_intersector(
              mesh_.vertices.data(), mesh_.faces.data(), sizeof(float) * 3);
          nanort::TriangleIntersection<float> isect;
          bool hit = gAccel.Traverse(ray, triangle_intersector, &isect);
          if (hit) {
            float3 p;
            p[0] = ray.org[0] + isect.t * ray.dir[0];
            p[1] = ray.org[1] + isect.t * ray.dir[1];
            p[2] = ray.org[2] + isect.t * ray.dir[2];

            buffer->position[4 * pidx + 0] = p.x();
            buffer->position[4 * pidx + 1] = p.y();
            buffer->position[4 * pidx + 2] = p.z();
            buffer->position[4 * pidx + 3] = 1.0f;

            unsigned int prim_id = isect.prim_id;

            float3 N;
            {
              unsigned int f0, f1, f2;
              f0 = mesh_.faces[3 * prim_id + 0];
              f1 = mesh_.faces[3 * prim_id + 1];
              f2 = mesh_.faces[3 * prim_id + 2];

              float3 v0, v1, v2;
              v0[0] = mesh_.vertices[3 * f0 + 0];
              v0[1] = mesh_.vertices[3 * f0 + 1];
              v0[2] = mesh_.vertices[3 * f0 + 2];
              v1[0] = mesh_.vertices[3 * f1 + 0];
              v1[1] = mesh_.vertices[3 * f1 + 1];
              v1[2] = mesh_.vertices[3 * f1 + 2];
              v2[0] = mesh_.vertices[3 * f2 + 0];
              v2[1] = mesh_.vertices[3 * f2 + 1];
              v2[2] = mesh_.vertices[3 * f2 + 2];
              CalcNormal(N, v0, v1, v2);
            }

            buffer->normal[4 * pidx + 0] = 0.5f * N[0] + 0.5f;
            buffer->normal[4 * pidx + 1] = 0.5f * N[1] + 0.5f;
            buffer->normal[4 * pidx + 2] = 0.5f * N[2] + 0.5f;
            buffer->normal[4 * pidx + 3] = 1.0f;

            buffer->depth[4 * pidx + 0] = isect.t;
            buffer->depth[4 * pidx + 1] = isect.t;
            buffer->depth[4 * pidx + 2] = isect.t;
            buffer->depth[4 * pidx + 3] = 1.0f;

            float3 UV;
            if (mesh_.uvs.size() > 0) {
              float3 uv0, uv1, uv2;
              uint32_t v0, v1, v2;
              v0 = mesh_.faces[3 * prim_id + 0];
              v1 = mesh_.faces[3 * prim_id + 1];
              v2 = mesh_.faces[3 * prim_id + 2];

              uv0[0] = mesh_.uvs[2 * v0 + 0];
              uv0[1] = mesh_.uvs[2 * v0 + 1];
              uv1[0] = mesh_.uvs[2 * v1 + 0];
              uv1[1] = mesh_.uvs[2 * v1 + 1];
              uv2[0] = mesh_.uvs[2 * v2 + 0];
              uv2[1] = mesh_.uvs[2 * v2 + 1];

              UV = Lerp3(uv0, uv1, uv2, isect.u, isect.v);

              buffer->texcoord[4 * pidx + 0] = UV[0];
              buffer->texcoord[4 * pidx + 1] = UV[1];
              buffer->texcoord[4 * pidx + 2] = 0.0f;
              buffer->texcoord[4 * pidx + 3] = 1.0f;
            }

            // Fetch texture
            float tex_col[3];
            // add global texture offset
            FetchTexture(image_, UV[0] + config.uv_offset[0], UV[1] + config.uv_offset[1], tex_col);

            // Use texture as diffuse color.
            buffer->diffuse[4 * pidx + 0] = tex_col[0];
            buffer->diffuse[4 * pidx + 1] = tex_col[1];
            buffer->diffuse[4 * pidx + 2] = tex_col[2];
            buffer->diffuse[4 * pidx + 3] = 1.0f;

            // Simple shading
            float NdotV = fabsf(vdot(N, dir));

            buffer->rgba[4 * pidx + 0] = NdotV * tex_col[0];
            buffer->rgba[4 * pidx + 1] = NdotV * tex_col[1];
            buffer->rgba[4 * pidx + 2] = NdotV * tex_col[2];
            buffer->rgba[4 * pidx + 3] = 1.0f;
          }
        }
      }
    }));
  }

  for (auto& t : workers) {
    t.join();
  }

  return true;
};

}  // namespace example
