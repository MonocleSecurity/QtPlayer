#include <array>
#include <chrono>
#include <optional>
#include <QOpenGLExtraFunctions>
#include <QOpenGLFunctions>
#include <QOpenGLWidget>
#include <stdint.h>

struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;

struct FRAME
{
  FRAME(GLuint framebuffer, GLuint texture, uint64_t time);

  GLuint framebuffer_;
  GLuint texture_;
  uint64_t time_;
};

class OpenGLWidget : public QOpenGLWidget, public QOpenGLExtraFunctions
{
  Q_OBJECT

  public:
    OpenGLWidget(QWidget* parent);
    ~OpenGLWidget();

  protected:
    virtual void initializeGL() override;
    virtual void resizeGL(int width, int height) override;
    virtual void paintGL() override;
    virtual void timerEvent(QTimerEvent*) override;

  private:
    GLuint yuv_shader_program_;
    GLuint rgb_shader_program_;
    std::array<GLuint, 3> yuv_textures_;
    GLuint yuv_vao_;
    GLuint yuv_vbo_;
    GLuint yuv_ebo_;
    GLuint rgb_vao_;
    GLuint rgb_vbo_;
    GLuint rgb_ebo_;
    std::vector<FRAME> frames_;
    std::vector<FRAME> free_frames_;

    AVFormatContext* format_context_;
    AVCodecContext* codec_context_;
    AVPacket* av_packet_;
    AVFrame* av_frame_;
    std::optional<unsigned int> video_stream_;
    double time_base_;
    std::chrono::steady_clock::time_point start_time_;
};
