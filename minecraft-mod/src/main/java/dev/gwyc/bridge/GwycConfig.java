package dev.gwyc.bridge;

import net.fabricmc.loader.api.FabricLoader;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Properties;

/**
 * Настройки мода: config/gwyc-bridge.properties.
 *
 * <p>Путь к каналу и его ёмкость обязаны совпадать с параметрами моста на стороне
 * игры (GwycBridgeConfig.channelPath и ringCapacity).
 */
public final class GwycConfig {

    /** Включён ли мост. Выключенный мод не открывает канал и ничего не отправляет. */
    public boolean enabled = true;

    /** Файл канала. Относительный путь считается от каталога запуска Minecraft. */
    public String channelPath = "gwyc_bridge.channel";

    /** Ёмкость кольца канала в байтах (степень двойки). */
    public int ringCapacity = 128 * 1024;

    /** Имя игрока, которому выдаются награды казино. Пусто — первый игрок на сервере. */
    public String rewardPlayer = "";

    /** Подробный лог в консоль сервера. */
    public boolean verbose = true;

    /** Максимальный размер региона для снимка (по каждой оси), чтобы кадр влез в лимит. */
    public int maxSnapshotSize = 24;

    public static Path configFile() {
        return FabricLoader.getInstance().getConfigDir().resolve("gwyc-bridge.properties");
    }

    public static GwycConfig load() {
        final GwycConfig config = new GwycConfig();
        final Path file = configFile();

        if (!Files.exists(file)) {
            config.save();
            return config;
        }

        final Properties properties = new Properties();
        try (InputStream input = Files.newInputStream(file)) {
            properties.load(input);
        } catch (IOException error) {
            System.err.println("[gwyc] не прочитать конфиг " + file + ": " + error.getMessage());
            return config;
        }

        config.enabled = Boolean.parseBoolean(properties.getProperty("enabled", "true"));
        config.channelPath = properties.getProperty("channelPath", config.channelPath);
        config.ringCapacity = parseInt(properties.getProperty("ringCapacity"), config.ringCapacity);
        config.rewardPlayer = properties.getProperty("rewardPlayer", "");
        config.verbose = Boolean.parseBoolean(properties.getProperty("verbose", "true"));
        config.maxSnapshotSize = parseInt(properties.getProperty("maxSnapshotSize"), config.maxSnapshotSize);

        return config;
    }

    public void save() {
        final Properties properties = new Properties();
        properties.setProperty("enabled", Boolean.toString(enabled));
        properties.setProperty("channelPath", channelPath);
        properties.setProperty("ringCapacity", Integer.toString(ringCapacity));
        properties.setProperty("rewardPlayer", rewardPlayer == null ? "" : rewardPlayer);
        properties.setProperty("verbose", Boolean.toString(verbose));
        properties.setProperty("maxSnapshotSize", Integer.toString(maxSnapshotSize));

        final Path file = configFile();
        try {
            Files.createDirectories(file.getParent());
            try (OutputStream output = Files.newOutputStream(file)) {
                properties.store(output, "Мост Gamble With Your Crafts ↔ Minecraft");
            }
        } catch (IOException error) {
            System.err.println("[gwyc] не записать конфиг " + file + ": " + error.getMessage());
        }
    }

    private static int parseInt(String text, int fallback) {
        if (text == null) {
            return fallback;
        }
        try {
            return Integer.parseInt(text.trim());
        } catch (NumberFormatException ignored) {
            return fallback;
        }
    }
}
