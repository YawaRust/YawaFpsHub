# Заголовки API видеокарт

Неизменённые публичные заголовки, без DLL, загрузчиков SDK или кода примеров.

- AMD ADL: https://github.com/GPUOpen-LibrariesAndSDKs/display-library
  commit `84c881cb2136f04287eb3af12de00a9e2867aa9e`, `include/*.h`.
  MIT; полный copyright и текст разрешения в каждом заголовке.
- Intel IGCL: https://github.com/intel/drivers.gpu.control-library
  commit `b6c462933502e13d1537dd5024949a51be30e63d`, `include/igcl_api.h`.
  Intel Software License Agreement; исходные LICENSE, License.txt и third-party-software.txt сохранены.
  Intel API вызывается только при наличии адаптера Intel, для его показателей.

Официальные типы нужны для точного ABI, а не для включения SDK runtime в приложение.
DLL берутся из System32 установленного графического драйвера. Их отсутствие и
неподдержанные поля дают «нет данных». Ни драйверы GPU, ни DLL вендоров не копируются.
Код HWiNFO не используется. Изменение частот, напряжений и вентилятора не вызывается.

Эти заголовки и условия включены также в `third_party/pawnio/sources.zip`, доступный
из exe через `--sensor-sources`. Клиент, сопоставление адаптеров и пересчёт энергии написаны отдельно.
