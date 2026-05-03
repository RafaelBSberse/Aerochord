#pragma once

#include <chrono>
#include <string>
#include <string_view>

namespace aerochord {

// =============================================================================
// Logger — logging estruturado com nível e tag de módulo
//
// Thread-safe: as implementações devem garantir que flush() seja atômico.
// Por padrão redireciona para stderr; pode ser substituído por um sink externo.
// =============================================================================

enum class LogLevel {
    DBG     = 0,
    INFO    = 1,
    WARNING = 2,
    ERR     = 3,
    FATAL   = 4,
};

class Logger {
public:
    // Obtém a instância singleton
    static Logger& instance();

    // Nível mínimo de log (mensagens abaixo são descartadas)
    void setLevel(LogLevel level);
    LogLevel level() const;

    // Registra uma mensagem com tag de módulo, nível e timestamp automático
    void log(LogLevel level,
             std::string_view module,
             std::string_view message) const;

    // Atalhos por nível
    void dbg    (std::string_view module, std::string_view message) const;
    void info   (std::string_view module, std::string_view message) const;
    void warning(std::string_view module, std::string_view message) const;
    void err    (std::string_view module, std::string_view message) const;
    void fatal  (std::string_view module, std::string_view message) const;

private:
    Logger() = default;

    LogLevel minLevel_{ LogLevel::DBG };
};

// ---------------------------------------------------------------------------
// Macros de conveniência — inserem automaticamente o nome do módulo
// ---------------------------------------------------------------------------
#define AEROCHORD_LOG_DEBUG(module, msg)   \
    ::aerochord::Logger::instance().dbg(module, msg)

#define AEROCHORD_LOG_INFO(module, msg)    \
    ::aerochord::Logger::instance().info(module, msg)

#define AEROCHORD_LOG_WARN(module, msg)    \
    ::aerochord::Logger::instance().warning(module, msg)

#define AEROCHORD_LOG_ERROR(module, msg)   \
    ::aerochord::Logger::instance().err(module, msg)

#define AEROCHORD_LOG_FATAL(module, msg)   \
    ::aerochord::Logger::instance().fatal(module, msg)

} // namespace aerochord
