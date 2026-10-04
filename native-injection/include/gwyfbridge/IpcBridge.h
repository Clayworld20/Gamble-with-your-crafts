// ============================================================================
//  IpcBridge.h — отображение региона разделяемой памяти и события WinAPI.
//
//  Кольца и правила публикации живут в IpcProtocol (переносимая часть), здесь
//  же только то, что привязано к Windows: CreateFileMappingW, события, проверка
//  живости партнёрского процесса. Каждое направление обслуживает один канал;
//  сторона выбирает свой канал по роли.
// ============================================================================
#pragma once

#include "Common.h"
#include "IpcProtocol.h"

namespace gwyf::ipc {

/// Роль процесса в обмене. Определяет, какие каналы он продюсирует.
enum class Role {
    /// Внутри игры Gamble With Your Friends.
    Game,
    /// Внутри JVM Minecraft.
    Minecraft,
};

/// Полноценный мост: регион + события + два канала.
class Bridge {
public:
    Bridge() = default;
    Bridge(const Bridge&) = delete;
    Bridge& operator=(const Bridge&) = delete;
    ~Bridge();

    /// Создать/открыть регион и события. createIfMissing должен быть true
    /// у того, кто стартует первым (обычно это игра), и false у партнёра,
    /// чтобы не появилось два независимых региона.
    bool Open(Role role, const std::wstring& regionName, const std::string& targetTag,
              bool createIfMissing, std::string* error = nullptr);

    /// Закрыть каналы, записать Bye, освободить объекты.
    void Close();

    [[nodiscard]] bool IsOpen() const { return header_ != nullptr; }
    [[nodiscard]] SharedHeader* Header() const { return header_; }

    /// Продюсер «моей» стороны.
    [[nodiscard]] RecordProducer& Out() { return out_; }

    /// Потребитель входящих записей.
    [[nodiscard]] RecordConsumer& In() { return in_; }

    /// Роль этого процесса.
    [[nodiscard]] Role MyRole() const { return role_; }

    /// Партнёр жив? (проверка по пульсу).
    [[nodiscard]] bool PeerAlive() const;

    /// Живёт ли партнёр именно как процесс (защита от «зомби»-региона).
    [[nodiscard]] bool PeerProcessExists() const;

    /// Запрошена ли остановка (инжектор создал событие Stop).
    [[nodiscard]] bool StopRequested() const;

    /// Строка диагностики для логов.
    [[nodiscard]] std::string Describe() const;

private:
    Role role_ = Role::Game;
    SharedHeader* header_ = nullptr;
    MappedFile region_;
    UniqueHandle eventGameToMc_;
    UniqueHandle eventMcToGame_;
    UniqueHandle eventStop_;
    RecordProducer out_;
    RecordConsumer in_;
};

/// Отправить команду партнёру и (опционально) дождаться Ack.
bool SendCommand(RecordProducer& producer, CommandCode code, std::string_view text = {}, u32 arg0 = 0, u32 arg1 = 0);

}  // namespace gwyf::ipc
