// =============================================================================
//  RingBuffer.h — SPSC (single producer / single consumer) кольцевой буфер
//  без блокировок. Живёт в разделяемой памяти: одна сторона пишет, другая читает.
//
//  Требования к корректности:
//    * producer и consumer — РАЗНЫЕ потоки/процессы, каждый ровно один;
//    * capacity — степень двойки, поэтому индекс = pos & mask (без деления);
//    * head/tail — atomic<uint32_t> с acquire/release: производитель публикует
//      данные release-записью tail, потребитель забирает их acquire-чтением;
//    * при полном буфере Write() возвращает false и НИКОГДА не блокирует
//      вызывающий поток: игровой поток нельзя останавливать на IPC.
// =============================================================================
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace gwyc {

/// Разделяемый между процессами заголовок кольца.
struct RingControlBlock {
    /// Позиция записи (растёт монотонно, не маскируется) — пишет только producer.
    std::atomic<uint32_t> tail;
    /// Позиция чтения (растёт монотонно) — пишет только consumer.
    std::atomic<uint32_t> head;
    /// Счётчик переполнений: сообщения терялись (диагностика, не логика).
    std::atomic<uint32_t> overruns;
    /// Ёмкость буфера в байтах (степень двойки).
    uint32_t capacity;
    uint32_t reserved;
};

static_assert(std::is_trivially_copyable_v<RingControlBlock>, "RingControlBlock должен быть POD");
static_assert(sizeof(RingControlBlock) == 20, "RingControlBlock должен занимать 20 байт во всех процессах");

/// Кольцо байт поверх уже выделенной памяти (см. SharedChannel: он её мапит).
class RingBuffer {
public:
    RingBuffer() noexcept = default;

    /// Привязать кольцо к памяти: controlBlock и data должны жить в разделяемой области.
    void Attach(RingControlBlock* controlBlock, uint8_t* data, uint32_t capacity) noexcept
    {
        m_control = controlBlock;
        m_data = data;
        m_capacity = capacity;
        m_mask = (capacity != 0) ? (capacity - 1u) : 0u;
    }

    /// Размер разделяемой области, требуемый под кольцо с данной ёмкостью.
    [[nodiscard]] static constexpr size_t BytesRequired(uint32_t capacity) noexcept
    {
        return sizeof(RingControlBlock) + capacity;
    }

    /// Ёмкость приводится к степени двойки, не меньше 1 КиБ и не больше 64 МиБ.
    [[nodiscard]] static constexpr uint32_t NormalizeCapacity(uint32_t requested) noexcept
    {
        uint32_t capacity = 1024;
        const uint32_t limit = 64u * 1024u * 1024u;
        while (capacity < requested && capacity < limit) capacity <<= 1;
        return capacity;
    }

    [[nodiscard]] bool IsAttached() const noexcept { return m_control != nullptr && m_data != nullptr; }

    /// Сколько байт доступно для чтения.
    [[nodiscard]] uint32_t ReadableBytes() const noexcept
    {
        if (!IsAttached()) return 0;
        const uint32_t tail = m_control->tail.load(std::memory_order_acquire);
        const uint32_t head = m_control->head.load(std::memory_order_relaxed);
        return tail - head;
    }

    /// Сколько байт можно записать. Ёмкость-1, чтобы «полный» и «пустой» не путались.
    [[nodiscard]] uint32_t WritableBytes() const noexcept
    {
        if (!IsAttached()) return 0;
        const uint32_t tail = m_control->tail.load(std::memory_order_relaxed);
        const uint32_t head = m_control->head.load(std::memory_order_acquire);
        return (m_capacity - 1u) - (tail - head);
    }

    /// Записать блок. false — не влез целиком (частичной записи не бывает).
    [[nodiscard]] bool Write(const void* data, uint32_t size) noexcept
    {
        if (!IsAttached()) return false;
        if (size == 0) return true;
        if (size > WritableBytes()) return false;

        const uint32_t tail = m_control->tail.load(std::memory_order_relaxed);
        const uint32_t offset = tail & m_mask;
        const uint32_t firstChunk = (m_capacity - offset < size) ? (m_capacity - offset) : size;
        const uint32_t secondChunk = size - firstChunk;

        std::memcpy(m_data + offset, data, firstChunk);
        if (secondChunk != 0) {
            std::memcpy(m_data, static_cast<const uint8_t*>(data) + firstChunk, secondChunk);
        }

        // release: весь memcpy выше обязан быть виден читателю до публикации tail.
        m_control->tail.store(tail + size, std::memory_order_release);
        return true;
    }

    /// То же, что Write, но при переполнении инкрементирует счётчик overrun.
    [[nodiscard]] bool TryWrite(const void* data, uint32_t size) noexcept
    {
        if (IsAttached() && size > 0 && size > WritableBytes()) {
            NoteOverrun();
            return false;
        }
        return Write(data, size);
    }

    /// Зафиксировать потерянное сообщение (вызывается, когда писать уже некуда).
    void NoteOverrun() noexcept
    {
        if (IsAttached()) m_control->overruns.fetch_add(1u, std::memory_order_relaxed);
    }

    /// Прочитать до size байт. Возвращает число прочитанных байт.
    uint32_t Read(void* destination, uint32_t size) noexcept
    {
        if (!IsAttached() || size == 0) return 0;

        const uint32_t head = m_control->head.load(std::memory_order_relaxed);
        const uint32_t tail = m_control->tail.load(std::memory_order_acquire);
        const uint32_t available = tail - head;
        if (available == 0) return 0;

        const uint32_t toRead = (size < available) ? size : available;
        const uint32_t offset = head & m_mask;
        const uint32_t firstChunk = (m_capacity - offset < toRead) ? (m_capacity - offset) : toRead;
        const uint32_t secondChunk = toRead - firstChunk;

        std::memcpy(destination, m_data + offset, firstChunk);
        if (secondChunk != 0) {
            std::memcpy(static_cast<uint8_t*>(destination) + firstChunk, m_data, secondChunk);
        }

        // release: производитель не должен перезаписать прочитанное раньше времени.
        m_control->head.store(head + toRead, std::memory_order_release);
        return toRead;
    }

    /// Посмотреть до size байт, НЕ сдвигая позицию чтения.
    ///
    /// Безопасно для SPSC: head меняет только потребитель, а производитель не может
    /// перезаписать область [head, tail) — его собственный предел это учитывает.
    [[nodiscard]] bool Peek(uint32_t offset, void* destination, uint32_t size) const noexcept
    {
        if (!IsAttached()) return false;
        const uint32_t head = m_control->head.load(std::memory_order_relaxed);
        const uint32_t tail = m_control->tail.load(std::memory_order_acquire);
        if (tail - head < offset + size) return false;

        const uint32_t start = (head + offset) & m_mask;
        const uint32_t firstChunk = (m_capacity - start < size) ? (m_capacity - start) : size;
        const uint32_t secondChunk = size - firstChunk;

        std::memcpy(destination, m_data + start, firstChunk);
        if (secondChunk != 0) {
            std::memcpy(static_cast<uint8_t*>(destination) + firstChunk, m_data, secondChunk);
        }
        return true;
    }

    /// Пропустить count байт (после Peek).
    void Skip(uint32_t count) noexcept
    {
        if (!IsAttached() || count == 0) return;
        const uint32_t head = m_control->head.load(std::memory_order_relaxed);
        const uint32_t tail = m_control->tail.load(std::memory_order_acquire);
        const uint32_t available = tail - head;
        const uint32_t toSkip = (count < available) ? count : available;
        m_control->head.store(head + toSkip, std::memory_order_release);
    }

    [[nodiscard]] uint32_t Overruns() const noexcept
    {
        return IsAttached() ? m_control->overruns.load(std::memory_order_relaxed) : 0u;
    }

private:
    RingControlBlock* m_control = nullptr;
    uint8_t* m_data = nullptr;
    uint32_t m_capacity = 0;
    uint32_t m_mask = 0;
};

/// Сбросить кольцо в исходное состояние. Вызывать только когда обе стороны стоят.
inline void ResetRing(RingControlBlock* controlBlock) noexcept
{
    if (controlBlock == nullptr) return;
    controlBlock->head.store(0, std::memory_order_relaxed);
    controlBlock->tail.store(0, std::memory_order_relaxed);
    controlBlock->overruns.store(0, std::memory_order_relaxed);
}

} // namespace gwyc
