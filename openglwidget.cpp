#include "openglwidget.h"
#include <QApplication>
#include <QFileDialog>
#include <QMessageBox>
#include <QTimer>
extern "C"
{
  #include <libavformat/avformat.h>
  #include <libavcodec/avcodec.h>
}

FRAME::FRAME(GLuint framebuffer, GLuint texture, uint64_t time) :
  framebuffer_(framebuffer),
  texture_(texture),
  time_(time)
{
}

OpenGLWidget::OpenGLWidget(QWidget* parent) :
  QOpenGLWidget(parent),
  yuv_shader_program_(GL_INVALID_VALUE),
  rgb_shader_program_(GL_INVALID_VALUE),
  yuv_textures_({ GL_INVALID_VALUE, GL_INVALID_VALUE, GL_INVALID_VALUE }),
  yuv_vao_(GL_INVALID_VALUE),
  yuv_vbo_(GL_INVALID_VALUE),
  yuv_ebo_(GL_INVALID_VALUE),
  rgb_vao_(GL_INVALID_VALUE),
  rgb_vbo_(GL_INVALID_VALUE),
  rgb_ebo_(GL_INVALID_VALUE),
  format_context_(nullptr),
  codec_context_(nullptr),
  av_packet_(av_packet_alloc()),
  av_frame_(av_frame_alloc()),
  time_base_(0.0)
{
  // Open a file
  const QString filename = QFileDialog::getOpenFileName(window(), "Open File", QDir::homePath(), "All Files (*.*)");
  if (filename.isEmpty())
  {
    QMessageBox::critical(this, "Error", "No file selected: " + filename);
    return;
  }
  // Open the file with FFMPEG
  if (avformat_open_input(&format_context_, filename.toUtf8().constData(), nullptr, nullptr) < 0)
  {
    QMessageBox::critical(this, "Error", "Invalid file: " + filename);
    return;
  }
  if (avformat_find_stream_info(format_context_, nullptr) < 0)
  {
    QMessageBox::critical(this, "Error", "Failed to find stream info: " + filename);
    return;
  }
  for (unsigned int i = 0; i < format_context_->nb_streams; i++)
  {
    if (format_context_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
    {
      video_stream_ = i;
      break;
    }
  }
  if (!video_stream_.has_value())
  {
    QMessageBox::critical(this, "Error", "Failed to find video stream: " + filename);
    return;
  }
  // Open the decoder
  const AVCodec* codec = avcodec_find_decoder(format_context_->streams[*video_stream_]->codecpar->codec_id);
  if (codec == nullptr)
  {
    QMessageBox::critical(this, "Error", "Failed to find video stream decoder: " + filename);
    return;
  }
  codec_context_ = avcodec_alloc_context3(codec);
  if (codec_context_ == nullptr)
  {
    QMessageBox::critical(this, "Error", "Failed to allocate video stream context: " + filename);
    return;
  }
  // Copy codec parameters
  if (avcodec_parameters_to_context(codec_context_, format_context_->streams[*video_stream_]->codecpar) < 0)
  {
    QMessageBox::critical(this, "Error", "Failed to copy video stream context: " + filename);
    return;
  }
  if (avcodec_open2(codec_context_, codec, nullptr) < 0)
  {
    QMessageBox::critical(this, "Error", "Failed to open video stream context: " + filename);
    return;
  }
  window()->setFixedSize(format_context_->streams[*video_stream_]->codecpar->width, format_context_->streams[*video_stream_]->codecpar->height);
}

OpenGLWidget::~OpenGLWidget()
{
  // OpenGL
  makeCurrent();
  glDeleteShader(yuv_shader_program_);
  glDeleteShader(rgb_shader_program_);
  glDeleteTextures(yuv_textures_.size(), yuv_textures_.data());
  glDeleteVertexArrays(1, &yuv_vao_);
  glDeleteBuffers(1, &yuv_vbo_);
  glDeleteBuffers(1, &yuv_ebo_);
  glDeleteVertexArrays(1, &rgb_vao_);
  glDeleteBuffers(1, &rgb_vbo_);
  glDeleteBuffers(1, &rgb_ebo_);
  for (const FRAME& frame : frames_)
  {
    glDeleteFramebuffers(1, &frame.framebuffer_);
    glDeleteTextures(1, &frame.texture_);
  }
  for (const FRAME& frame : free_frames_)
  {
    glDeleteFramebuffers(1, &frame.framebuffer_);
    glDeleteTextures(1, &frame.texture_);
  }
  doneCurrent();
  // Codec
  avformat_free_context(format_context_);
  avcodec_free_context(&codec_context_);
  av_packet_free(&av_packet_);
  av_frame_free(&av_frame_);
  video_stream_.reset();
}

void OpenGLWidget::initializeGL()
{
  initializeOpenGLFunctions();
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  // YUV Shaders
  const char* yuv_vertex_shader_source = R"(#version 330 core
                                            layout(location = 0) in vec2 in_pos;
                                            layout(location = 1) in vec2 in_tex_coord;
                                            out vec2 tex_coord;
                                            void main()
                                            {
                                                gl_Position = vec4(in_pos, 0.0, 1.0);
                                                tex_coord = in_tex_coord;
                                            })";
  const char* yuv_fragment_shader_source = R"(#version 330 core
                                              out vec4 FragColor;
                                              in vec2 tex_coord;
                                              uniform sampler2D texture_y;
                                              uniform sampler2D texture_u;
                                              uniform sampler2D texture_v;
                                              void main()
                                              {
                                                  float y = texture(texture_y, tex_coord).r;
                                                  float u = texture(texture_u, tex_coord).r - 0.5;
                                                  float v = texture(texture_v, tex_coord).r - 0.5;
                                                  vec3 rgb = mat3(1.0, 1.0, 1.0,
                                                                  0.0, -0.39465, 2.03211,
                                                                  1.13983, -0.58060, 0.0) * vec3(y, u, v);
                                                  FragColor = vec4(rgb, 1.0);
                                              })";
  const GLuint vertex_shader = glCreateShader(GL_VERTEX_SHADER);
  glShaderSource(vertex_shader, 1, &yuv_vertex_shader_source, nullptr);
  glCompileShader(vertex_shader);
  const GLuint fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
  glShaderSource(fragment_shader, 1, &yuv_fragment_shader_source, nullptr);
  glCompileShader(fragment_shader);
  yuv_shader_program_ = glCreateProgram();
  glAttachShader(yuv_shader_program_, vertex_shader);
  glAttachShader(yuv_shader_program_, fragment_shader);
  glLinkProgram(yuv_shader_program_);
  glDeleteShader(vertex_shader);
  glDeleteShader(fragment_shader);
  // Grab dimensions from the codec
  const int width = format_context_->streams[*video_stream_]->codecpar->width;
  const int height = format_context_->streams[*video_stream_]->codecpar->height;
  for (int i = 0; i < 5; ++i)
  {
    GLuint frame_buffer = GL_INVALID_VALUE;
    glGenFramebuffers(1, &frame_buffer);
    glBindFramebuffer(GL_FRAMEBUFFER, frame_buffer);

    GLuint texture = GL_INVALID_VALUE;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      throw std::runtime_error("Framebuffer is not complete!");
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    free_frames_.push_back(FRAME(frame_buffer, texture, 0));
  }
  glGenTextures(3, yuv_textures_.data());
  for (int i = 0; i < 3; i++)
  {
    glBindTexture(GL_TEXTURE_2D, yuv_textures_[i]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }
  glBindTexture(GL_TEXTURE_2D, yuv_textures_[0]);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
  glBindTexture(GL_TEXTURE_2D, yuv_textures_[1]);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, width / 2, height / 2, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
  glBindTexture(GL_TEXTURE_2D, yuv_textures_[2]);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, width / 2, height / 2, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
  // RGB shader
  const char* rgb_vertex_shader_source = R"(#version 330 core
                                            layout(location = 0) in vec2 in_pos;
                                            layout(location = 1) in vec2 in_tex_coord;
                                            out vec2 tex_coord;
                                            void main()
                                            {
                                                gl_Position = vec4(in_pos, 0.0, 1.0);
                                                tex_coord = in_tex_coord;
                                            })";
  const char* rgb_fragment_shader_source = R"(#version 330 core
                                              out vec4 FragColor;
                                              in vec2 tex_coord;
                                              uniform sampler2D rgb_texture;
                                              void main()
                                              {
                                                  FragColor = texture(rgb_texture, tex_coord);
                                              })";
  GLuint rgb_vertex_shader = glCreateShader(GL_VERTEX_SHADER);
  glShaderSource(rgb_vertex_shader, 1, &rgb_vertex_shader_source, nullptr);
  glCompileShader(rgb_vertex_shader);
  GLuint rgb_fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
  glShaderSource(rgb_fragment_shader, 1, &rgb_fragment_shader_source, nullptr);
  glCompileShader(rgb_fragment_shader);
  rgb_shader_program_ = glCreateProgram();
  glAttachShader(rgb_shader_program_, rgb_vertex_shader);
  glAttachShader(rgb_shader_program_, rgb_fragment_shader);
  glLinkProgram(rgb_shader_program_);
  glDeleteShader(rgb_vertex_shader);
  glDeleteShader(rgb_fragment_shader);
  // YUV Geometory
  const float yuv_vertices[] =
  {
    // positions  texture coords
    -1.0f, -1.0f, 0.0f, 0.0f,
     1.0f, -1.0f, 1.0f, 0.0f,
     1.0f,  1.0f, 1.0f, 1.0f,
    -1.0f,  1.0f, 0.0f, 1.0f
  };
  const unsigned int yuv_indices[] =
  {
    0, 1, 2,
    2, 3, 0
  };
  glGenVertexArrays(1, &yuv_vao_);
  glGenBuffers(1, &yuv_vbo_);
  glGenBuffers(1, &yuv_ebo_);
  glBindVertexArray(yuv_vao_);
  glBindBuffer(GL_ARRAY_BUFFER, yuv_vbo_);
  glBufferData(GL_ARRAY_BUFFER, sizeof(yuv_vertices), yuv_vertices, GL_STATIC_DRAW);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, yuv_ebo_);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(yuv_indices), yuv_indices, GL_STATIC_DRAW);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void*>(0));
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void*>(2 * sizeof(float)));
  glEnableVertexAttribArray(1);
  glBindVertexArray(0);
  // RGB Geometory
  const float rgb_vertices[] =
  {
    // positions  texture coords
    -1.0f, -1.0f, 0.0f, 1.0f,
     1.0f, -1.0f, 1.0f, 1.0f,
     1.0f,  1.0f, 1.0f, 0.0f,
    -1.0f,  1.0f, 0.0f, 0.0f
  };
  const unsigned int rgb_indices[] =
  {
    0, 1, 2,
    2, 3, 0
  };
  glGenVertexArrays(1, &rgb_vao_);
  glGenBuffers(1, &rgb_vbo_);
  glGenBuffers(1, &rgb_ebo_);
  glBindVertexArray(rgb_vao_);
  glBindBuffer(GL_ARRAY_BUFFER, rgb_vbo_);
  glBufferData(GL_ARRAY_BUFFER, sizeof(rgb_vertices), rgb_vertices, GL_STATIC_DRAW);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, rgb_ebo_);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(rgb_indices), rgb_indices, GL_STATIC_DRAW);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void*>(0));
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void*>(2 * sizeof(float)));
  glEnableVertexAttribArray(1);
  glBindVertexArray(0);
  // Start update timer
  startTimer(16);
  time_base_ = static_cast<double>(format_context_->streams[*video_stream_]->time_base.num) / static_cast<double>(format_context_->streams[*video_stream_]->time_base.den) * 1000.0;
  start_time_ = std::chrono::steady_clock::now();
  timerEvent(nullptr);
}

void OpenGLWidget::resizeGL(int width, int height)
{
}

void OpenGLWidget::paintGL()
{
  glClear(GL_COLOR_BUFFER_BIT);
  // Find the best frame to display
  const uint64_t current_time = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time_).count();
  std::vector<FRAME>::iterator current_frame = frames_.end();
  for (std::vector<FRAME>::iterator frame = frames_.begin(); frame != frames_.end(); ++frame)
  {
    if (current_time < frame->time_)
    {
      break;
    }
    current_frame = frame;
  }
  if (current_frame == frames_.end())
  {
    return;
  }
  // Display it
  glViewport(0, 0, format_context_->streams[*video_stream_]->codecpar->width, format_context_->streams[*video_stream_]->codecpar->height);
  glUseProgram(rgb_shader_program_);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, current_frame->texture_);
  glUniform1i(glGetUniformLocation(rgb_shader_program_, "rgb_texture"), 0);
  // Draw
  glBindVertexArray(rgb_vao_);
  glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
  // Clear up
  glBindVertexArray(0);
  glUseProgram(0);
}

void OpenGLWidget::timerEvent(QTimerEvent*)
{
  // Just update every time
  update();
  // Figure out if we need to display the next frame
  const uint64_t current_time = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time_).count();
  // Read packets and decode frames
  while (frames_.empty() || (current_time > frames_.back().time_))
  {
    int ret = av_read_frame(format_context_, av_packet_);
    if (ret && (ret != AVERROR_EOF))
    {
      return;
    }
    else
    {
      if (av_packet_->stream_index != *video_stream_)
      {
        continue;
      }
      // Send packets
      if (avcodec_send_packet(codec_context_, av_packet_))
      {
        return;
      }
    }
    // Collect frames
    while (true)
    {
      if (avcodec_receive_frame(codec_context_, av_frame_))
      {
        break;
      }
      // Find a free frame
      std::optional<FRAME> frame;
      if (free_frames_.empty())
      {
        // Clear up any old frames we can to make some space
        std::vector<FRAME>::iterator current_frame = frames_.end();
        for (std::vector<FRAME>::iterator frame = frames_.begin(); frame != frames_.end(); ++frame)
        {
          if (current_time < frame->time_)
          {
            break;
          }
          current_frame = frame;
        }
        if (current_frame == frames_.end())
        {
          // We've failed to find any frames, lets just give up
          return;
        }
        std::for_each(frames_.begin(), current_frame, [this](const FRAME& frame) { free_frames_.push_back(frame); });
        frames_.erase(frames_.begin(), current_frame);
        // Grab one
        frame = free_frames_.back();
        free_frames_.pop_back();
      }
      else
      {
        frame = free_frames_.back();
        free_frames_.pop_back();
      }
      frame->time_ = static_cast<uint64_t>(static_cast<double>(av_frame_->pts) * time_base_);
      frames_.push_back(*frame);
      // Update textures with AVFrame data
      makeCurrent();
      // Bind frame buffer and shader
      glBindFramebuffer(GL_FRAMEBUFFER, frame->framebuffer_);
      glUseProgram(yuv_shader_program_);
      // Bind textures
      glBindTexture(GL_TEXTURE_2D, yuv_textures_[0]);
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, av_frame_->width, av_frame_->height, GL_RED, GL_UNSIGNED_BYTE, av_frame_->data[0]);
      glBindTexture(GL_TEXTURE_2D, yuv_textures_[1]);
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, av_frame_->width / 2, av_frame_->height / 2, GL_RED, GL_UNSIGNED_BYTE, av_frame_->data[1]);
      glBindTexture(GL_TEXTURE_2D, yuv_textures_[2]);
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, av_frame_->width / 2, av_frame_->height / 2, GL_RED, GL_UNSIGNED_BYTE, av_frame_->data[2]);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, yuv_textures_[0]);
      glUniform1i(glGetUniformLocation(yuv_shader_program_, "texture_y"), 0);
      glActiveTexture(GL_TEXTURE1);
      glBindTexture(GL_TEXTURE_2D, yuv_textures_[1]);
      glUniform1i(glGetUniformLocation(yuv_shader_program_, "texture_u"), 1);
      glActiveTexture(GL_TEXTURE2);
      glBindTexture(GL_TEXTURE_2D, yuv_textures_[2]);
      glUniform1i(glGetUniformLocation(yuv_shader_program_, "texture_v"), 2);
      // Draw
      glBindVertexArray(yuv_vao_);
      glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
      glBindVertexArray(0);
      // Clean up
      glUseProgram(0);
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      doneCurrent();
    }
  }
}
