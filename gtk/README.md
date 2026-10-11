# GTK launcher

Нативный GTK4/libadwaita интерфейс для GNOME. Загрузка vanilla-версии, список
экземпляров, офлайн-имя игрока, запуск Java и удаление экземпляра работают без
внешнего лаунчера. Уже созданные vanilla-экземпляры сохраняются при обновлении.

## Сборка

Нужны GTK 4.6+, libadwaita 1.5+, zlib, gettext и CMake 3.25+ (в Debian/Ubuntu:
`libgtk-4-dev libadwaita-1-dev zlib1g-dev gettext`):

```sh
cmake -S gtk -B build-gtk
cmake --build build-gtk
```

Исполняемый файл: `build-gtk/svinlauncher`.

Для запуска требуется Java в `PATH`. При создании экземпляра клиент напрямую
загружает version JSON, client JAR, библиотеки, native-библиотеки и ассеты с серверов
Minecraft. Для самой загрузки требуется сетевой доступ.

Каталог данных по умолчанию — `$XDG_DATA_HOME/SvinLauncher` или
`~/.local/share/SvinLauncher`. При первом запуске старый каталог данных автоматически
переносится; путь можно переопределить через `SVINLAUNCHER_DATA_DIR`. Настройка
`InstanceDir` из `svinlauncher.cfg` учитывается.
