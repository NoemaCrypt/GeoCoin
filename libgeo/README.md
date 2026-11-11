# libgeo

Кроссплатформенная библиотека `libgeo` с C API для Android/iOS/Windows и других клиентов.

## Что уже есть

- Проверка адреса (`GEO...`/`GEOM...`)
- Производный адрес из приватного ключа
- Подпись/проверка/recover для 32-байтового digest (recoverable payload)
- Создание и проверка multisig-адреса
- Транзакционный crypto API: tx hash, подпись/проверка входа, recover подписанта входа
- Объектный Transaction API: сборка tx, сериализация/десериализация, hash, подпись/verify

## Сборка ядра

```bash
cmake -S libgeo -B libgeo/build
cmake --build libgeo/build -j4
```

Результат: `libgeo/build/libgeo.so` (при `LIBGEO_BUILD_SHARED=ON`).

## Публичный C API

Заголовок: `libgeo/include/libgeo/libgeo.h`

Базовые функции:

- `geo_api_version`
- `geo_last_error`
- `geo_verify_address`
- `geo_derive_address_from_private_key`
- `geo_sign_digest32_recoverable`
- `geo_verify_signed_message`
- `geo_recover_signer_address`
- `geo_multisig_create`
- `geo_multisig_verify`
- `geo_tx_hash_from_raw`
- `geo_tx_sign_input`
- `geo_tx_verify_input_signature`
- `geo_tx_recover_input_signer_address`
- `geo_tx_new` / `geo_tx_free`
- `geo_tx_deserialize` / `geo_tx_serialize`
- `geo_tx_set_header`
- `geo_tx_add_input` / `geo_tx_add_output`
- `geo_tx_set_input_signature`
- `geo_tx_sign_input_inplace`
- `geo_tx_hash`
- `geo_tx_verify` / `geo_tx_verify_input`

## Android JNI

Папка: `libgeo/bindings/android`

- JNI bridge: `src/main/cpp/libgeo_jni.cpp`
- Java API: `src/main/java/com/geocoin/libgeo/LibGeo.java`
- CMake: `CMakeLists.txt`

Схема: приложение загружает `libgeo` и `geo_jni`, затем работает через `LibGeo`.

## iOS Swift

Папка: `libgeo/bindings/ios`

- Swift package: `Package.swift`
- C module target: `Sources/CLibGeo` (`libgeo.h`, `module.modulemap`)
- Swift wrapper: `Sources/LibGeoSwift/LibGeo.swift`

`LibGeoSwift` дает Swift API поверх C функций из `libgeo`.
