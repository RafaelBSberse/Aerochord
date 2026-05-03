#include <gtest/gtest.h>
#include "common/LockFreeQueue.h"

using namespace aerochord;

// ---------------------------------------------------------------------------
// Testes básicos de push/pop
// ---------------------------------------------------------------------------

TEST(LockFreeQueue, PushAndPop) {
    LockFreeQueue<int, 4> q;
    EXPECT_TRUE(q.push(42));
    auto v = q.pop();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 42);
}

TEST(LockFreeQueue, PopEmpty) {
    LockFreeQueue<int, 4> q;
    EXPECT_FALSE(q.pop().has_value());
}

// ---------------------------------------------------------------------------
// Overflow: push além da capacidade retorna false
// ---------------------------------------------------------------------------

TEST(LockFreeQueue, FullQueue) {
    LockFreeQueue<int, 4> q;
    EXPECT_TRUE(q.push(1));
    EXPECT_TRUE(q.push(2));
    EXPECT_TRUE(q.push(3));
    EXPECT_TRUE(q.push(4));
    EXPECT_FALSE(q.push(5));   // fila cheia — item descartado
    EXPECT_FALSE(q.push(6));   // permanece cheia
}

TEST(LockFreeQueue, FullThenPopThenPush) {
    LockFreeQueue<int, 2> q;
    EXPECT_TRUE(q.push(10));
    EXPECT_TRUE(q.push(20));
    EXPECT_FALSE(q.push(30));  // cheia

    EXPECT_EQ(*q.pop(), 10);   // libera slot
    EXPECT_TRUE(q.push(30));   // agora aceita
    EXPECT_EQ(*q.pop(), 20);
    EXPECT_EQ(*q.pop(), 30);
    EXPECT_FALSE(q.pop().has_value());
}

// ---------------------------------------------------------------------------
// Ordem FIFO
// ---------------------------------------------------------------------------

TEST(LockFreeQueue, FifoOrder) {
    LockFreeQueue<int, 8> q;
    for (int i = 0; i < 5; ++i) EXPECT_TRUE(q.push(i));
    for (int i = 0; i < 5; ++i) EXPECT_EQ(*q.pop(), i);
    EXPECT_FALSE(q.pop().has_value());
}

// ---------------------------------------------------------------------------
// Reutilização: push/pop em múltiplos ciclos
// ---------------------------------------------------------------------------

TEST(LockFreeQueue, MultipleRounds) {
    LockFreeQueue<int, 4> q;
    for (int round = 0; round < 10; ++round) {
        EXPECT_TRUE(q.push(round));
        EXPECT_EQ(*q.pop(), round);
    }
}

// ---------------------------------------------------------------------------
// Tipo não-trivial (move-only)
// ---------------------------------------------------------------------------

TEST(LockFreeQueue, MoveOnlyType) {
    LockFreeQueue<std::unique_ptr<int>, 4> q;
    EXPECT_TRUE(q.push(std::make_unique<int>(99)));
    auto v = q.pop();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(**v, 99);
}
