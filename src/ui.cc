// UI loop

#include "ui.h"

#include "stb_image_write.h"

#include "gui/render-buffer.h"
#include "gui/render.h"
#include "gui/trackball.h"

#include "gui/glfw/include/GLFW/glfw3.h"
#include "gui/imgui/imgui.h"
#include "gui/imgui/imgui_impl_glfw_gl2.h"

#include <cmath>
#include <iostream>
#include <mutex>

namespace prnet {

struct UIParameters {
  float showDepthRange[2] = {1400.0f, 1700.0f};  // Good for fov 8
  bool showDepthPeseudoColor = false;
  int showBufferMode = example::SHOW_BUFFER_COLOR;
};

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
#pragma clang diagnostic ignored "-Wglobal-constructors"
#endif

static example::RenderBuffer gRenderBuffer;
static UIParameters gUIParam;

static float gCurrQuat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
static float gPrevQuat[4] = {0.0f, 0.0f, 0.0f, 1.0f};

static example::Renderer gRenderer;

static std::atomic<bool> gRenderQuit;
static std::atomic<bool> gRenderRefresh;
static example::RenderConfig gRenderConfig;
static std::mutex gMutex;

#ifdef __clang__
#pragma clang diagnostic pop
#endif

template <typename T>
inline T clamp(T f, T fmin, T fmax) {
  return std::max(std::min(fmax, f), fmin);
}

//
// Assume pixel values are basially in the range of [0.0, 1.0].
//
static bool SaveRGBAImageAsPNG(const std::string &filename,
                               const std::vector<float> &src, int width,
                               int height, bool gamma) {
  std::vector<uint8_t> image;

  image.resize(size_t(width * height) * 3);

  for (size_t y = 0; y < size_t(height); y++) {
    for (size_t x = 0; x < size_t(width); x++) {
      size_t dst_idx = (size_t(height) - y - 1) * size_t(width) + x; // flip Y
      size_t src_idx = y * size_t(width) + x;
      if (gamma) {
        // apply gamma correction
        image[3 * dst_idx + 0] = static_cast<uint8_t>(
            clamp(std::pow(src[4 * src_idx + 0], 1.0f / 2.2f) * 255.0f, 0.0f, 255.0f));
        image[3 * dst_idx + 1] = static_cast<uint8_t>(
            clamp(std::pow(src[4 * src_idx + 1], 1.0f / 2.2f) * 255.0f, 0.0f, 255.0f));
        image[3 * dst_idx + 2] = static_cast<uint8_t>(
            clamp(std::pow(src[4 * src_idx + 2], 1.0f / 2.2f) * 255.0f, 0.0f, 255.0f));
      } else {
        // linear
        image[3 * dst_idx + 0] =
            static_cast<uint8_t>(clamp(src[4 * src_idx + 0] * 255.0f, 0.0f, 255.0f));
        image[3 * dst_idx + 1] =
            static_cast<uint8_t>(clamp(src[4 * src_idx + 1] * 255.0f, 0.0f, 255.0f));
        image[3 * dst_idx + 2] =
            static_cast<uint8_t>(clamp(src[4 * src_idx + 2] * 255.0f, 0.0f, 255.0f));
      }
    }
  }

  // Save
  int ret = stbi_write_png(filename.c_str(), width, height, 3, &image.at(0),
                           /* stride_in_bytes */ width * 3);

  return (ret > 0) ? true : false;
}

static void SaveBuffers(const example::RenderBuffer &buffer, int width, int height) {
  std::string color_filename = "buffer_color.png";
  std::string texture_filename = "buffer_texture.png";
  std::string normal_filename = "buffer_normal.png";

  if (!SaveRGBAImageAsPNG(color_filename, buffer.rgba, width, height,
                          /* gamma */ true)) {
    std::cerr << "Failed to write " << color_filename << std::endl;
  } else {
    std::cout << "Wrote " << color_filename << std::endl;
  }
  

  if (!SaveRGBAImageAsPNG(texture_filename, buffer.rgba, width, height,
                          /* gamma */ true)) {
    std::cerr << "Failed to write " << texture_filename << std::endl;
  } else {
    std::cout << "Wrote " << texture_filename << std::endl;
  }

  {
    std::vector<float> normal;
    normal.resize(size_t(width * height) * 4);

    for (size_t i = 0; i < size_t(width * height); i++) {
      // [-1, 1] -> [0, 1]
      normal[4 * i + 0] = buffer.normal[4 * i + 0] * 0.5f + 0.5f;
      normal[4 * i + 1] = buffer.normal[4 * i + 1] * 0.5f + 0.5f;
      normal[4 * i + 2] = buffer.normal[4 * i + 2] * 0.5f + 0.5f;
      normal[4 * i + 3] = 1.0f;  // not used
    }

    if (!SaveRGBAImageAsPNG(normal_filename, normal, width, height,
                            /* gamma */ false)) {
      std::cerr << "Failed to write " << normal_filename << std::endl;
    } else {
      std::cout << "Wrote " << normal_filename << std::endl;
    }
  }
}

static void RequestRender() {
  {
    std::lock_guard<std::mutex> guard(gMutex);
    gRenderConfig.pass = 0;
  }

  gRenderRefresh = true;
}

static void RenderThread() {
  {
    std::lock_guard<std::mutex> guard(gMutex);
    gRenderConfig.pass = 0;
  }

  while (1) {
    if (gRenderQuit) {
      std::cout << "Quit render thread." << std::endl;
      return;
    }

    if (!gRenderRefresh || gRenderConfig.pass >= gRenderConfig.max_passes) {
      // Give some cycles to this thread.
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }

    // auto startT = std::chrono::system_clock::now();

    // Initialize display buffer for the first pass.
    bool initial_pass = false;
    {
      std::lock_guard<std::mutex> guard(gMutex);
      if (gRenderConfig.pass == 0) {
        initial_pass = true;
      }
    }

    {
      std::lock_guard<std::mutex> guard(gMutex);
      bool ret = gRenderer.Render(&gRenderBuffer, gCurrQuat, gRenderConfig);

      if (ret) {

        gRenderConfig.pass++;
      }
    }

    // auto endT = std::chrono::system_clock::now();

    gRenderRefresh = false;

    // std::chrono::duration<double, std::milli> ms = endT - startT;
    // std::cout << ms.count() << " [ms]\n";
  }
}

static void error_callback(int error, const char *description) {
  std::cerr << "GLFW Error " << error << ", " << description << std::endl;
}

inline float pseudoColor(float v, int ch) {
  if (ch == 0) {  // red
    if (v <= 0.5f)
      return 0.f;
    else if (v < 0.75f)
      return (v - 0.5f) / 0.25f;
    else
      return 1.f;
  } else if (ch == 1) {  // green
    if (v <= 0.25f)
      return v / 0.25f;
    else if (v < 0.75f)
      return 1.f;
    else
      return 1.f - (v - 0.75f) / 0.25f;
  } else if (ch == 2) {  // blue
    if (v <= 0.25f)
      return 1.f;
    else if (v < 0.5f)
      return 1.f - (v - 0.25f) / 0.25f;
    else
      return 0.f;
  } else {  // alpha
    return 1.f;
  }
}

static void Display(int width, int height, int buffer_mode,
                    const example::RenderBuffer &buffer) {
  std::vector<float> buf(size_t(width * height * 4));
  if (buffer_mode == example::SHOW_BUFFER_COLOR) {
    // TODO: normalize
    for (size_t i = 0; i < buf.size() / 4; i++) {
      buf[4 * i + 0] = std::pow(buffer.rgba[4 * i + 0], 1.0f / 2.2f);
      buf[4 * i + 1] = std::pow(buffer.rgba[4 * i + 1], 1.0f / 2.2f);
      buf[4 * i + 2] = std::pow(buffer.rgba[4 * i + 2], 1.0f / 2.2f);
      buf[4 * i + 3] = buffer.rgba[4 * i + 3]; // no gamma correction for alpha
    }
  } else if (buffer_mode == example::SHOW_BUFFER_NORMAL) {
    for (size_t i = 0; i < buf.size(); i++) {
      buf[i] = buffer.normal[i];
    }
  } else if (buffer_mode == example::SHOW_BUFFER_POSITION) {
    for (size_t i = 0; i < buf.size(); i++) {
      buf[i] = buffer.position[i];
    }
  } else if (buffer_mode == example::SHOW_BUFFER_DEPTH) {
    float d_min =
        std::min(gUIParam.showDepthRange[0], gUIParam.showDepthRange[1]);
    float d_diff =
        std::fabs(gUIParam.showDepthRange[1] - gUIParam.showDepthRange[0]);
    d_diff = std::max(d_diff, std::numeric_limits<float>::epsilon());
    for (size_t i = 0; i < buf.size(); i++) {
      float v = (buffer.depth[i] - d_min) / d_diff;
      if (gUIParam.showDepthPeseudoColor) {
        buf[i] = pseudoColor(v, i % 4);
      } else {
        buf[i] = v;
      }
    }
  } else if (buffer_mode == example::SHOW_BUFFER_TEXCOORD) {
    for (size_t i = 0; i < buf.size() / 4; i++) {
      buf[4 * i + 0] = std::pow(buffer.texcoord[4 * i + 0], 1.0f / 2.2f);
      buf[4 * i + 1] = std::pow(buffer.texcoord[4 * i + 1], 1.0f / 2.2f);
      buf[4 * i + 2] = std::pow(buffer.texcoord[4 * i + 2], 1.0f / 2.2f);
      buf[4 * i + 3] = buffer.rgba[4 * i + 3]; // no gamma correction for alpha
    }
  } else if (buffer_mode == example::SHOW_BUFFER_DIFFUSE) {
    for (size_t i = 0; i < buf.size() / 4; i++) {
      buf[4 * i + 0] = std::pow(buffer.diffuse[4 * i + 0], 1.0f / 2.2f);
      buf[4 * i + 1] = std::pow(buffer.diffuse[4 * i + 1], 1.0f / 2.2f);
      buf[4 * i + 2] = std::pow(buffer.diffuse[4 * i + 2], 1.0f / 2.2f);
      buf[4 * i + 3] = buffer.diffuse[4 * i + 2];
    }
  }

  glRasterPos2i(-1, -1);
  glDrawPixels(width, height, GL_RGBA, GL_FLOAT,
               static_cast<const GLvoid *>(&buf.at(0)));
}

static void HandleUserInput(GLFWwindow *window, const double view_width,
                            const double view_height, double *prev_mouse_x,
                            double *prev_mouse_y) {
  ImGuiIO &io = ImGui::GetIO();
  if (io.WantCaptureMouse || io.WantCaptureKeyboard) {
    return;
  }

  // Handle mouse input
  double mouse_x, mouse_y;
  glfwGetCursorPos(window, &mouse_x, &mouse_y);
  if (int(mouse_x) == int(*prev_mouse_x) &&
      int(mouse_y) == int(*prev_mouse_y)) {
    return;
  }

  int window_width, window_height;
  glfwGetWindowSize(window, &window_width, &window_height);
  // const double width = static_cast<double>(window_width);
  const double height = static_cast<double>(window_height);

  const double kTransScale = 0.05;
  const double kZoomScale = 0.75;

  if (ImGui::IsMouseDown(0)) {  // left mouse button

    if (glfwGetKey(window, GLFW_KEY_T) == GLFW_PRESS) {
      // T for translation

      gRenderConfig.eye[0] -= kTransScale * (mouse_x - (*prev_mouse_x));
      gRenderConfig.eye[1] -= kTransScale * (mouse_y - (*prev_mouse_y));
      gRenderConfig.look_at[0] -= kTransScale * (mouse_x - (*prev_mouse_x));
      gRenderConfig.look_at[1] -= kTransScale * (mouse_y - (*prev_mouse_y));

      RequestRender();

    } else if (glfwGetKey(window, GLFW_KEY_Z) == GLFW_PRESS) {
      // Z for zoom(dolly)

      gRenderConfig.eye[2] += kZoomScale * (mouse_y - (*prev_mouse_y));
      gRenderConfig.look_at[2] += kZoomScale * (mouse_y - (*prev_mouse_y));

      RequestRender();

    } else {
      // No key for rotation

      // Assume render view is located in lower-left.
      double offset_y = height - view_height;

      trackball(
          gPrevQuat, float((2.0 * (*prev_mouse_x) - view_width) / view_width),
          float((height - 2.0 * ((*prev_mouse_y) - offset_y)) / view_height),
          float((2.0 * mouse_x - view_width) / view_width),
          float((height - 2.0 * (mouse_y - offset_y)) / view_height));
      add_quats(gPrevQuat, gCurrQuat, gCurrQuat);

      RequestRender();
    }
  }

  // Update mouse coordinates
  *prev_mouse_x = mouse_x;
  *prev_mouse_y = mouse_y;
}

static int CreateHDRTextureGL(const Image<float> &image, int prev_id = -1) {
  const size_t width = image.getWidth();
  const size_t height = image.getHeight();
  const size_t n_channel = image.getChannels();

  GLuint id;
  if (prev_id < 0) {
    // Generate new texture
    glGenTextures(1, &id);
  } else {
    // Using previous texture
    id = static_cast<GLuint>(prev_id);
  }

  GLint last_texture;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &last_texture);

  glBindTexture(GL_TEXTURE_2D, id);
  // glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  // glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

  GLenum format = GL_RGBA;
  if (n_channel == 1) {
    format = GL_LUMINANCE;
  } else if (n_channel == 2) {
    format = GL_LUMINANCE_ALPHA;
  } else if (n_channel == 3) {
    format = GL_RGB;
  } else if (n_channel == 4) {
    format = GL_RGBA;
  } else {
    std::cerr << "Unknown the number of channels" << std::endl;
    return prev_id;
  }

  // gamma correction.
  std::vector<float> dst;
  dst.resize(width * height * n_channel);

  if (n_channel == 1) {
    for (size_t i = 0; i < width * height; i++) {
      dst[i] = std::pow(image.getData()[i], 1.0f / 2.2f);
    }
  } else if (n_channel == 2) {
    for (size_t i = 0; i < width * height; i++) {
      dst[2 * i + 0] = std::pow(image.getData()[2 * i + 0], 1.0f / 2.2f);
      dst[2 * i + 1] = image.getData()[2 * i + 1];
    }
  } else if (n_channel == 3) {
    for (size_t i = 0; i < width * height; i++) {
      dst[3 * i + 0] = std::pow(image.getData()[3 * i + 0], 1.0f / 2.2f);
      dst[3 * i + 1] = std::pow(image.getData()[3 * i + 1], 1.0f / 2.2f);
      dst[3 * i + 2] = std::pow(image.getData()[3 * i + 2], 1.0f / 2.2f);
    }
  } else if (n_channel == 4) {
    for (size_t i = 0; i < width * height; i++) {
      dst[4 * i + 0] = std::pow(image.getData()[4 * i + 0], 1.0f / 2.2f);
      dst[4 * i + 1] = std::pow(image.getData()[4 * i + 1], 1.0f / 2.2f);
      dst[4 * i + 2] = std::pow(image.getData()[4 * i + 2], 1.0f / 2.2f);
      dst[4 * i + 3] = image.getData()[4 * i + 3];
    }
  }
  
  

  if (prev_id < 0) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, int(width), int(height), 0, format, GL_FLOAT,
                 reinterpret_cast<const void *>(dst.data()));
  } else {
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, int(width), int(height), format, GL_FLOAT,
                    reinterpret_cast<const void *>(dst.data()));
  }

  glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(last_texture));

  return static_cast<int>(id);
}

bool RunUI(const Mesh &mesh, const Mesh &front_mesh,
           const Image<float> &input_image,
           const std::vector<Image<float>> &debug_images) {
  // Setup window
  glfwSetErrorCallback(error_callback);
  if (!glfwInit()) {
    std::cerr << "Failed to initialize glfw" << std::endl;
    return false;
  }
  GLFWwindow *window =
      glfwCreateWindow(1280, 720, "PRNet infer", nullptr, nullptr);
  glfwMakeContextCurrent(window);
  glfwSwapInterval(1);  // Enable vsync

  // Setup ImGui binding
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  ImGui_ImplGlfwGL2_Init(window, true);

  // Load Dock
  // ImGui::LoadDock();

  io.Fonts->AddFontDefault();

  // Setup style
  ImGui::StyleColorsDark();

  // Setup rendering settings;
  gRenderConfig.eye[0] = 0.0f;
  gRenderConfig.eye[1] = 0.0f;
  gRenderConfig.eye[2] = 1500.0f;

  gRenderConfig.look_at[0] = 0.0f;
  gRenderConfig.look_at[1] = 0.0f;
  gRenderConfig.look_at[2] = 0.0f;

  gRenderConfig.up[0] = 0.0f;
  gRenderConfig.up[1] = 1.0f;
  gRenderConfig.up[2] = 0.0f;

  gRenderConfig.width = 512;
  gRenderConfig.height = 512;

  gRenderConfig.fov = 8.0f;

  gRenderConfig.max_passes = 1;

  gRenderBuffer.resize(size_t(gRenderConfig.width),
                       size_t(gRenderConfig.height));

  trackball(gCurrQuat, 0.0f, 0.0f, 0.0f, 0.0f);

  // Setup renderer.
  gRenderer.SetMesh(mesh);
  gRenderer.SetImage(input_image);
  gRenderer.BuildBVH();

  // Launch render thread
  gRenderQuit = false;
  std::thread renderThread(RenderThread);

  // trigger first rendering
  RequestRender();

  std::vector<int> debug_image_texs;
  for (size_t i = 0; i < debug_images.size(); i++) {
      const int id = CreateHDRTextureGL(debug_images[i]);
      debug_image_texs.push_back(id);
  }

  // Main loop
  double mouse_x = 0, mouse_y = 0;
#ifdef USE_DLIB
  bool use_front_mesh = false;
#endif
  while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();
    ImGui_ImplGlfwGL2_NewFrame();

    // Ctrl + q to exit
    if (glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS &&
        glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS) {
      // Exit application
      break;
    }

    // space to reset rotation
    if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS) {
      trackball(gCurrQuat, 0.0f, 0.0f, 0.0f, 0.0f);
      gRenderConfig.eye[0] = 0.0f;
      gRenderConfig.eye[1] = 0.0f;
      gRenderConfig.eye[2] = 1500.0f;
      gRenderConfig.look_at[0] = 0.0f;
      gRenderConfig.look_at[1] = 0.0f;
      gRenderConfig.look_at[2] = 0.0f;
      gRenderConfig.up[0] = 0.0f;
      gRenderConfig.up[1] = 1.0f;
      gRenderConfig.up[2] = 0.0f;
      gRenderConfig.fov = 8.0f;

      RequestRender();
    }

    // Handle user's mouse and key input
    HandleUserInput(window, double(gRenderConfig.width),
                    double(gRenderConfig.height), &mouse_x, &mouse_y);

    // ImGui
    ImGui::Begin("UI");
    {
      if (ImGui::Button("Save buffers")) {
        std::lock_guard<std::mutex> guard(gMutex);
        SaveBuffers(gRenderBuffer, gRenderConfig.width, gRenderConfig.height);
      }

      ImGui::RadioButton("color", &(gUIParam.showBufferMode),
                         example::SHOW_BUFFER_COLOR);
      ImGui::SameLine();
      ImGui::RadioButton("normal", &(gUIParam.showBufferMode),
                         example::SHOW_BUFFER_NORMAL);
      ImGui::SameLine();
      ImGui::RadioButton("position", &(gUIParam.showBufferMode),
                         example::SHOW_BUFFER_POSITION);
      ImGui::SameLine();
      ImGui::RadioButton("depth", &(gUIParam.showBufferMode),
                         example::SHOW_BUFFER_DEPTH);
      ImGui::SameLine();
      ImGui::RadioButton("texcoord", &(gUIParam.showBufferMode),
                         example::SHOW_BUFFER_TEXCOORD);
      ImGui::SameLine();
      ImGui::RadioButton("diffuse(texture)", &(gUIParam.showBufferMode),
                         example::SHOW_BUFFER_DIFFUSE);

      ImGui::InputFloat2("show depth range", gUIParam.showDepthRange);
      ImGui::Checkbox("show depth pesudo color",
                      &gUIParam.showDepthPeseudoColor);

      if (ImGui::InputFloat3("eye", gRenderConfig.eye)) {
        RequestRender();
      }

      if (ImGui::DragFloat2("UV offset", gRenderConfig.uv_offset, 0.001f, 0.0f,
                            1.0f)) {
        RequestRender();
      }
      if (ImGui::DragFloat("fov", &(gRenderConfig.fov), 0.01f, 0.01f, 120.0f)) {
        RequestRender();
      }

#ifdef USE_DLIB
      if (ImGui::Checkbox("frontalized mesh", &use_front_mesh)) {
        // Switch mesh
        if (use_front_mesh) {
          gRenderer.SetMesh(front_mesh);
          gRenderer.BuildBVH();
        } else {
          gRenderer.SetMesh(mesh);
          gRenderer.BuildBVH();
        }
        RequestRender();
      }
#else
      (void)front_mesh;
#endif

    }
    ImGui::End();

    ImGui::Begin("Debug Images");
    {
      for (size_t i = 0; i < debug_image_texs.size(); i++) {
        // Show debug image
        const ImTextureID imgui_tex_id =reinterpret_cast<void *>(
            static_cast<intptr_t>(static_cast<int>(debug_image_texs[i])));
        const int img_width = int(debug_images[i].getWidth());
        const int img_height = int(debug_images[i].getHeight());
        const int width = int(ImGui::GetWindowHeight()) - 10;
        const int height = img_height * width / img_width;
        ImGui::Image(imgui_tex_id, ImVec2(width, height), ImVec2(0, 0),
                     ImVec2(1,1), ImVec4(1, 1, 1, 1), ImVec4(1, 1, 1, 0.5));
      }
    }
    ImGui::End();

    // Display rendered image.
    int display_w, display_h;
    glfwGetFramebufferSize(window, &display_w, &display_h);
    glViewport(0, 0, display_w, display_h);
    glClearColor(0.45f, 0.55f, 0.60f, 1.00f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    Display(gRenderConfig.width, gRenderConfig.height, gUIParam.showBufferMode,
            gRenderBuffer);

    // ImGui Display
    // glUseProgram(0); // You may want this if using this code in an OpenGL
    // 3+ context where shaders may be bound, but prefer using the GL3+ code.
    ImGui::Render();
    ImGui_ImplGlfwGL2_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(window);
  }

  // Cleanup
  // ImGui::SaveDock();

  gRenderQuit = true;
  renderThread.join();

  ImGui_ImplGlfwGL2_Shutdown();
  ImGui::DestroyContext();
  glfwTerminate();

  return true;
}

}  // namespace prnet
