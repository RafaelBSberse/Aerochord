#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <optional>
#include <utility>

namespace aerochord {

// =============================================================================
// LockFreeQueue<T, Capacity>
//
// Fila SPSC (Single-Producer / Single-Consumer) lock-free baseada em
// ring buffer de capacidade fixa em tempo de compilação.
//
// Garantias:
//   - push() chamado apenas pela thread produtora
//   - pop()  chamado apenas pela thread consumidora
//   - Sem alocações de heap após construção
//   - Sem mutexes; usa apenas operações atômicas com memory_order adequado
//
// Restrições:
//   - Capacity deve ser potência de 2 (verificado em static_assert)
//   - T deve ser move-constructible
// =============================================================================
template <typename T, size_t Capacity = 256>
class LockFreeQueue {
    static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0,
                  "Capacity deve ser potência de 2");

public:
    LockFreeQueue() = default;

    // Não copiável, não movível (contém atomics e array fixo)
    LockFreeQueue(const LockFreeQueue&)            = delete;
    LockFreeQueue& operator=(const LockFreeQueue&) = delete;

    // -------------------------------------------------------------------------
    // push — chamado pela thread produtora
    // Retorna false se a fila estiver cheia (item descartado).
    // -------------------------------------------------------------------------
    bool push(const T& item) {
        return emplace(item);
    }

    bool push(T&& item) {
        return emplace(std::move(item));
    }

    // -------------------------------------------------------------------------
    // pop — chamado pela thread consumidora
    // Retorna std::nullopt se a fila estiver vazia.
    // -------------------------------------------------------------------------
    std::optional<T> pop() {
        const size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire))
            return std::nullopt;  // vazia

        std::optional<T> result{ std::move(buffer_[head & kMask]) };
        head_.store(head + 1, std::memory_order_release);
        return result;
    }

    // -------------------------------------------------------------------------
    // Consultas de estado (aproximadas — sem sincronização extra)
    // -------------------------------------------------------------------------
    bool isEmpty() const {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    bool isFull() const {
        const size_t tail = tail_.load(std::memory_order_acquire);
        return (tail - head_.load(std::memory_order_acquire)) == Capacity;
    }

    size_t size() const {
        const size_t tail = tail_.load(std::memory_order_acquire);
        const size_t head = head_.load(std::memory_order_acquire);
        return tail - head;
    }

    static constexpr size_t capacity() { return Capacity; }

private:
    static constexpr size_t kMask = Capacity - 1;

    template <typename U>
    bool emplace(U&& item) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if ((tail - head_.load(std::memory_order_acquire)) == Capacity)
            return false;  // cheia

        buffer_[tail & kMask] = std::forward<U>(item);
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Padding para evitar false sharing entre head_ e tail_ em diferentes threads
    alignas(64) std::atomic<size_t> head_{ 0 };
    alignas(64) std::atomic<size_t> tail_{ 0 };
    std::array<T, Capacity> buffer_{};
};

} // namespace aerochord
