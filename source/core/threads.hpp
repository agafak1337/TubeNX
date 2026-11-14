// threads.hpp — фоновые потоки с явным размером стека.
//
// Зачем: в devkitA64/newlib размер стека pthread по умолчанию невелик, а
// mbedTLS во время TLS-хендшейка и парсеры InnerTube реально используют
// сотни килобайт. Переполнение стека на Switch не даёт внятного исключения —
// процесс просто умирает. Поэтому все сетевые потоки запускаем сами.
#pragma once

#include <cstddef>
#include <functional>

namespace tnx::threads {

// 1 МиБ — с запасом на mbedTLS + HTTP + nlohmann-подобный разбор.
constexpr size_t kDefaultStack = 1024u * 1024u;

// Отсоединённый поток: сам удалится после завершения функции.
void spawn(std::function<void()> fn, size_t stack = kDefaultStack);

// Присоединяемый поток (для пула воркеров, который потом останавливают).
class Thread {
public:
    Thread() = default;
    ~Thread();

    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;
    Thread(Thread&& other) noexcept;
    Thread& operator=(Thread&& other) noexcept;

    bool start(std::function<void()> fn, size_t stack = kDefaultStack);
    void join();
    bool joinable() const { return task_ != nullptr; }

private:
    void* task_ = nullptr;
    unsigned long handle_ = 0;   // pthread_t как число: заголовок не тянем
};

} // namespace tnx::threads