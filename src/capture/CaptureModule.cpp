#include "CaptureModule.h"
#include "common/Logger.h"
#include "common/ThreadUtils.h"

// ---------------------------------------------------------------------------
// Backend V4L2 (Linux)
// ---------------------------------------------------------------------------
#ifdef __linux__
#  include <cerrno>
#  include <cstring>
#  include <fcntl.h>
#  include <poll.h>
#  include <sys/ioctl.h>
#  include <sys/mman.h>
#  include <unistd.h>
#  include <linux/videodev2.h>
#endif

#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace aerochord {

static constexpr std::string_view kModule = "CaptureModule";

// ===========================================================================
// CameraHandle — backend V4L2 em Linux; struct vazio em outros sistemas
// ===========================================================================
#ifdef __linux__

struct CaptureModule::CameraHandle {
    int  fd        = -1;
    bool streaming = false;

    struct MmapBuffer {
        void*  start  = nullptr;
        size_t length = 0;
    };
    std::vector<MmapBuffer> buffers;
};

// ioctl com retry em EINTR (sinal de SO não fatal)
static int xioctl(int fd, unsigned long request, void* arg) {
    int r;
    do { r = ioctl(fd, request, arg); } while (r == -1 && errno == EINTR);
    return r;
}

// ---------------------------------------------------------------------------
// Conversão YUYV (YUV 4:2:2 packed) → BGR24
//
// Cada grupo de 4 bytes (Y0 U Y1 V) produz 2 pixels BGR.
// Coeficientes BT.601 com aritmética inteira para desempenho em tempo real.
// ---------------------------------------------------------------------------
static void yuyv2bgr(const uint8_t* src, uint8_t* dst, int width, int height) {
    const int pairs = (width * height) / 2;

    auto clamp = [](int v) -> uint8_t {
        return (v < 0) ? 0u : (v > 255) ? 255u : static_cast<uint8_t>(v);
    };

    auto toBgr = [&](int y, int d, int e, uint8_t* out) {
        const int c = y - 16;
        out[0] = clamp((298*c + 516*d           + 128) >> 8);  // B
        out[1] = clamp((298*c - 100*d - 208*e   + 128) >> 8);  // G
        out[2] = clamp((298*c           + 409*e + 128) >> 8);  // R
    };

    for (int i = 0; i < pairs; ++i) {
        const int y0 = src[4*i];
        const int d  = src[4*i + 1] - 128;  // U − 128
        const int y1 = src[4*i + 2];
        const int e  = src[4*i + 3] - 128;  // V − 128
        toBgr(y0, d, e, dst + 6*i);
        toBgr(y1, d, e, dst + 6*i + 3);
    }
}

#else  // macOS / Windows / outros: stub sem dependência de plataforma

struct CaptureModule::CameraHandle {};

#endif  // __linux__

// ===========================================================================
// Construção / destruição
// ===========================================================================
CaptureModule::CaptureModule(std::shared_ptr<FrameQueue> outputQueue)
    : CaptureModule(std::move(outputQueue), Config{}) {}

CaptureModule::CaptureModule(std::shared_ptr<FrameQueue> outputQueue,
                             Config config)
    : outputQueue_(std::move(outputQueue))
    , config_(config)
    , camera_(std::make_unique<CameraHandle>())
{
}

CaptureModule::~CaptureModule() {
    stop();
}

// ===========================================================================
// Ciclo de vida
// ===========================================================================
bool CaptureModule::start() {
    if (running_.load())
        return true;

    AEROCHORD_LOG_INFO(kModule, "Iniciando captura de vídeo...");

    if (!openCamera())
        return false;

    running_.store(true);
    thread_ = std::thread(&CaptureModule::captureLoop, this);

    AEROCHORD_LOG_INFO(kModule, "Thread de captura iniciada.");
    return true;
}

void CaptureModule::stop() {
    if (!running_.exchange(false))
        return;

    if (thread_.joinable())
        thread_.join();

    closeCamera();
    AEROCHORD_LOG_INFO(kModule, "Captura encerrada.");
}

bool CaptureModule::isRunning() const { return running_.load(); }

// ===========================================================================
// Diagnóstico
// ===========================================================================
uint64_t CaptureModule::framesDropped()  const { return framesDropped_.load(); }
uint64_t CaptureModule::framesCaptured() const { return framesCaptured_.load(); }
int      CaptureModule::actualFps()      const { return actualFps_.load(); }

// ===========================================================================
// openCamera / closeCamera
// ===========================================================================
bool CaptureModule::openCamera() {
#ifdef __linux__
    const std::string devPath = "/dev/video" + std::to_string(config_.deviceIndex);

    camera_->fd = open(devPath.c_str(), O_RDWR | O_NONBLOCK);
    if (camera_->fd == -1) {
        AEROCHORD_LOG_ERROR(kModule,
            "Não foi possível abrir " + devPath + ": " + strerror(errno));
        return false;
    }

    // --- Verificar capabilities ---
    v4l2_capability cap{};
    if (xioctl(camera_->fd, VIDIOC_QUERYCAP, &cap) == -1) {
        AEROCHORD_LOG_ERROR(kModule,
            "VIDIOC_QUERYCAP falhou: " + std::string(strerror(errno)));
        ::close(camera_->fd); camera_->fd = -1;
        return false;
    }
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        AEROCHORD_LOG_ERROR(kModule, devPath + " não é um dispositivo de captura.");
        ::close(camera_->fd); camera_->fd = -1;
        return false;
    }
    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        AEROCHORD_LOG_ERROR(kModule, devPath + " não suporta streaming mmap.");
        ::close(camera_->fd); camera_->fd = -1;
        return false;
    }

    // --- Detectar resolução (auto ou manual) ---
    int reqW = config_.targetWidth;
    int reqH = config_.targetHeight;

    if (reqW <= 0 || reqH <= 0) {
        // Enumerar resoluções e seus FPS máximos via VIDIOC_ENUM_FRAMEINTERVALS.
        // Escolher a maior resolução que suporte pelo menos kMinAcceptableFps.
        constexpr int kMinAcceptableFps = 15;

        struct ResCandidate { int w, h, fps; };
        std::vector<ResCandidate> candidates;

        v4l2_frmsizeenum frmsize{};
        frmsize.pixel_format = V4L2_PIX_FMT_YUYV;
        frmsize.index = 0;

        while (xioctl(camera_->fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) == 0) {
            if (frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
                const int w = static_cast<int>(frmsize.discrete.width);
                const int h = static_cast<int>(frmsize.discrete.height);

                // Consultar FPS máximo para esta resolução
                int maxFps = 0;
                v4l2_frmivalenum frmival{};
                frmival.pixel_format = V4L2_PIX_FMT_YUYV;
                frmival.width  = frmsize.discrete.width;
                frmival.height = frmsize.discrete.height;
                frmival.index  = 0;

                while (xioctl(camera_->fd, VIDIOC_ENUM_FRAMEINTERVALS, &frmival) == 0) {
                    if (frmival.type == V4L2_FRMIVAL_TYPE_DISCRETE &&
                        frmival.discrete.numerator > 0)
                    {
                        const int fps = static_cast<int>(
                            frmival.discrete.denominator / frmival.discrete.numerator);
                        if (fps > maxFps) maxFps = fps;
                    } else if (frmival.type == V4L2_FRMIVAL_TYPE_STEPWISE ||
                               frmival.type == V4L2_FRMIVAL_TYPE_CONTINUOUS) {
                        if (frmival.stepwise.min.numerator > 0) {
                            const int fps = static_cast<int>(
                                frmival.stepwise.min.denominator / frmival.stepwise.min.numerator);
                            if (fps > maxFps) maxFps = fps;
                        }
                        break;
                    }
                    ++frmival.index;
                }

                if (maxFps == 0) maxFps = 30;  // fallback se driver não reporta

                AEROCHORD_LOG_DEBUG(kModule,
                    "Resolução: " + std::to_string(w) + "x" + std::to_string(h) +
                    " @ " + std::to_string(maxFps) + " FPS");
                candidates.push_back({ w, h, maxFps });
            }
            ++frmsize.index;
        }

        // Priorizar FPS mais alto; entre empates, maior resolução
        int bestW = 640, bestH = 480, bestFps = 0;
        for (const auto& c : candidates) {
            if (c.fps > bestFps ||
                (c.fps == bestFps && c.w * c.h > bestW * bestH))
            {
                bestW = c.w;
                bestH = c.h;
                bestFps = c.fps;
            }
        }

        reqW = bestW;
        reqH = bestH;
        AEROCHORD_LOG_INFO(kModule,
            "Resolução auto-selecionada: " + std::to_string(reqW) + "x" +
            std::to_string(reqH) + " (max " + std::to_string(bestFps) + " FPS)");
    }

    // --- Configurar formato YUYV ---
    v4l2_format fmt{};
    fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = static_cast<uint32_t>(reqW);
    fmt.fmt.pix.height      = static_cast<uint32_t>(reqH);
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field       = V4L2_FIELD_NONE;
    if (xioctl(camera_->fd, VIDIOC_S_FMT, &fmt) == -1) {
        AEROCHORD_LOG_ERROR(kModule,
            "VIDIOC_S_FMT falhou: " + std::string(strerror(errno)));
        ::close(camera_->fd); camera_->fd = -1;
        return false;
    }
    // Atualizar config com os valores efetivamente aceitos pelo driver
    config_.targetWidth  = static_cast<int>(fmt.fmt.pix.width);
    config_.targetHeight = static_cast<int>(fmt.fmt.pix.height);
    AEROCHORD_LOG_INFO(kModule,
        "Formato aceito: " + std::to_string(config_.targetWidth) + "x" +
        std::to_string(config_.targetHeight) + " YUYV");

    // --- Frame rate ---
    // targetFps = 0 → auto-detectar: primeiro ler o FPS atual da câmera,
    // depois tentar ajustar se um valor específico foi solicitado.
    v4l2_streamparm parm{};
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (config_.targetFps > 0) {
        // Solicitar FPS específico
        parm.parm.capture.timeperframe.numerator   = 1;
        parm.parm.capture.timeperframe.denominator = static_cast<uint32_t>(config_.targetFps);
        xioctl(camera_->fd, VIDIOC_S_PARM, &parm);
    }

    // Ler FPS efetivamente aceito pelo driver
    if (xioctl(camera_->fd, VIDIOC_G_PARM, &parm) == 0 &&
        parm.parm.capture.timeperframe.numerator > 0)
    {
        const int negotiatedFps = static_cast<int>(
            parm.parm.capture.timeperframe.denominator /
            parm.parm.capture.timeperframe.numerator);
        actualFps_.store(negotiatedFps > 0 ? negotiatedFps : 30);
        AEROCHORD_LOG_INFO(kModule,
            "FPS negociado com a câmera: " + std::to_string(actualFps_.load()));
    } else {
        actualFps_.store(config_.targetFps > 0 ? config_.targetFps : 30);
        AEROCHORD_LOG_INFO(kModule,
            "VIDIOC_G_PARM indisponível — usando FPS padrão: " +
            std::to_string(actualFps_.load()));
    }

    // --- Alocar buffers mmap ---
    v4l2_requestbuffers req{};
    req.count  = static_cast<uint32_t>(config_.circularBuffer);
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(camera_->fd, VIDIOC_REQBUFS, &req) == -1) {
        AEROCHORD_LOG_ERROR(kModule,
            "VIDIOC_REQBUFS falhou: " + std::string(strerror(errno)));
        ::close(camera_->fd); camera_->fd = -1;
        return false;
    }

    camera_->buffers.resize(req.count);
    for (uint32_t i = 0; i < req.count; ++i) {
        v4l2_buffer buf{};
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if (xioctl(camera_->fd, VIDIOC_QUERYBUF, &buf) == -1) {
            AEROCHORD_LOG_ERROR(kModule,
                "VIDIOC_QUERYBUF[" + std::to_string(i) + "] falhou.");
            ::close(camera_->fd); camera_->fd = -1;
            return false;
        }
        camera_->buffers[i].start =
            mmap(nullptr, buf.length, PROT_READ | PROT_WRITE,
                 MAP_SHARED, camera_->fd, buf.m.offset);
        camera_->buffers[i].length = buf.length;
        if (camera_->buffers[i].start == MAP_FAILED) {
            AEROCHORD_LOG_ERROR(kModule,
                "mmap[" + std::to_string(i) + "] falhou: " + strerror(errno));
            ::close(camera_->fd); camera_->fd = -1;
            return false;
        }
    }

    // --- Enfileirar todos os buffers ---
    for (uint32_t i = 0; i < static_cast<uint32_t>(camera_->buffers.size()); ++i) {
        v4l2_buffer buf{};
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if (xioctl(camera_->fd, VIDIOC_QBUF, &buf) == -1) {
            AEROCHORD_LOG_ERROR(kModule, "VIDIOC_QBUF inicial falhou.");
            ::close(camera_->fd); camera_->fd = -1;
            return false;
        }
    }

    // --- Iniciar streaming ---
    const v4l2_buf_type bufType = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(camera_->fd, VIDIOC_STREAMON, const_cast<v4l2_buf_type*>(&bufType)) == -1) {
        AEROCHORD_LOG_ERROR(kModule,
            "VIDIOC_STREAMON falhou: " + std::string(strerror(errno)));
        ::close(camera_->fd); camera_->fd = -1;
        return false;
    }

    camera_->streaming = true;
    AEROCHORD_LOG_INFO(kModule, "Câmera V4L2 aberta: " + devPath);
    return true;

#else
    AEROCHORD_LOG_WARN(kModule,
        "Backend de câmera não implementado nesta plataforma. "
        "Frames serão emitidos vazios para manter o pipeline ativo.");
    return true;
#endif
}

void CaptureModule::closeCamera() {
#ifdef __linux__
    if (!camera_ || camera_->fd == -1)
        return;

    if (camera_->streaming) {
        const v4l2_buf_type bufType = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(camera_->fd, VIDIOC_STREAMOFF, const_cast<v4l2_buf_type*>(&bufType));
        camera_->streaming = false;
    }

    for (auto& buf : camera_->buffers) {
        if (buf.start && buf.start != MAP_FAILED)
            munmap(buf.start, buf.length);
    }
    camera_->buffers.clear();

    ::close(camera_->fd);
    camera_->fd = -1;
    AEROCHORD_LOG_INFO(kModule, "Câmera V4L2 fechada.");
#endif
}

// ===========================================================================
// Loop de captura (thread dedicada)
// ===========================================================================
void CaptureModule::captureLoop() {
    aerochord::tryElevateThreadPriority();
    AEROCHORD_LOG_DEBUG(kModule, "captureLoop() iniciado.");

#ifdef __linux__
    while (running_.load()) {
        // poll() evita busy-wait; timeout 100 ms para verificar running_ periodicamente
        pollfd pfd{ camera_->fd, POLLIN, 0 };
        const int ret = poll(&pfd, 1, /*timeout_ms=*/100);

        if (ret == 0) continue;           // timeout — re-verifica running_
        if (ret == -1) {
            if (errno == EINTR) continue;
            AEROCHORD_LOG_ERROR(kModule, "poll() falhou: " + std::string(strerror(errno)));
            break;
        }

        // Remover frame da fila V4L2
        v4l2_buffer buf{};
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (xioctl(camera_->fd, VIDIOC_DQBUF, &buf) == -1) {
            if (errno == EAGAIN) continue;
            AEROCHORD_LOG_ERROR(kModule,
                "VIDIOC_DQBUF falhou: " + std::string(strerror(errno)));
            break;
        }

        // Montar VideoFrame (YUYV → BGR24)
        VideoFrame frame;
        frame.width     = config_.targetWidth;
        frame.height    = config_.targetHeight;
        frame.channels  = 3;
        frame.timestamp = std::chrono::steady_clock::now();
        frame.data.resize(static_cast<size_t>(frame.width * frame.height * 3));

        yuyv2bgr(static_cast<const uint8_t*>(camera_->buffers[buf.index].start),
                 frame.data.data(), frame.width, frame.height);

        // Re-enfileirar buffer na câmera imediatamente (antes de publicar)
        if (xioctl(camera_->fd, VIDIOC_QBUF, &buf) == -1) {
            AEROCHORD_LOG_ERROR(kModule, "VIDIOC_QBUF falhou.");
            break;
        }

        // Publicar na fila do pipeline
        if (!outputQueue_->push(std::move(frame))) {
            ++framesDropped_;
            AEROCHORD_LOG_DEBUG(kModule, "Fila de frames cheia — quadro descartado.");
        } else {
            ++framesCaptured_;
        }
    }

#else
    // Plataformas sem backend V4L2: emite frames vazios com timing correto
    // Permite que o restante do pipeline (incluindo stub do MediaPipe) opere
    const int stubFps = (config_.targetFps > 0) ? config_.targetFps : 30;
    actualFps_.store(stubFps);
    const auto frameDuration =
        std::chrono::microseconds(1'000'000 / stubFps);

    while (running_.load()) {
        const auto nextFrame = std::chrono::steady_clock::now() + frameDuration;

        VideoFrame frame;
        frame.width     = config_.targetWidth;
        frame.height    = config_.targetHeight;
        frame.channels  = 3;
        frame.timestamp = std::chrono::steady_clock::now();
        // data vazia: stub do MediaPipe não usa pixels

        if (!outputQueue_->push(std::move(frame)))
            ++framesDropped_;
        else
            ++framesCaptured_;

        std::this_thread::sleep_until(nextFrame);
    }
#endif

    AEROCHORD_LOG_DEBUG(kModule, "captureLoop() encerrado.");
}

} // namespace aerochord
