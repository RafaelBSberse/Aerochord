#pragma once

#include <thread>

#ifdef __linux__
#  include <pthread.h>
#  include <sched.h>
#endif

namespace aerochord {

// Eleva a prioridade da thread atual para tempo-real soft (SCHED_FIFO)
// ou, se sem permissão, para a prioridade máxima de SCHED_OTHER.
// Chamada best-effort: falha silenciosa se sem permissão (cap 5.2.3 Desafio 1).
inline bool tryElevateThreadPriority() {
#ifdef __linux__
    pthread_t self = pthread_self();

    // Tentar SCHED_FIFO com prioridade mínima (menos invasivo)
    sched_param param{};
    param.sched_priority = sched_get_priority_min(SCHED_FIFO);
    if (pthread_setschedparam(self, SCHED_FIFO, &param) == 0)
        return true;

    // Fallback: nice máximo dentro de SCHED_OTHER (prioridade -20 requer root)
    // Sem efeito prático sem permissão, mas registra a tentativa.
    param.sched_priority = 0;
    pthread_setschedparam(self, SCHED_OTHER, &param);
    return false;
#else
    return false;  // TODO: SetThreadPriority no Windows, thread QoS no macOS
#endif
}

} // namespace aerochord
