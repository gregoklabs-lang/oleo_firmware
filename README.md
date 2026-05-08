# Firmware ESP32 Base - BLE + Wi-Fi + AWS IoT

## Panorama general
Firmware base para `ESP32-C3` (`adafruit_qtpy_esp32c3`) orientado a reutilizarse en otros proyectos. Mantiene aprovisionamiento BLE, conexion Wi-Fi, OLED, almacenamiento local en NVS/SPIFFS y conectividad MQTT segura con AWS IoT Core.

## Alcance actual
- Provisioning BLE con payload de credenciales y metadatos del dispositivo.
- Conexion Wi-Fi con reconexion automatica.
- OLED como indicador local de estado.
- Persistencia de `device_id`, `user_id`, Wi-Fi y parametros AWS.
- Cliente MQTT TLS con certificados en SPIFFS.
- Publicacion base de `claim` y `heartbeat`.
- `device_kind` enviado en el `claim` MQTT con valor default definido en firmware.
- Publicacion de `sensor_registry` con metadata del sensor ambient.
- Publicacion de `telemetry` con lecturas reales del SHT45 y VPD calculado.

## Lo que ya no incluye
- Lecturas PPFD o cualquier sensor especifico.
- Discovery de sensores.
- Downlinks de `settings` o `setpoints`.
- Persistencia de configuraciones de negocio.

## Arquitectura
- **Dispositivo**: ESP32-C3 con BLE, Wi-Fi y OLED.
- **AWS IoT Core**: broker MQTT TLS con certificados X.509.
- **Supabase**: auth y base de datos desde app/backend.
- **App Flutter**: login, onboarding y envio de credenciales por BLE.

## Flujo operativo
1. Monta `SPIFFS` y carga configuracion local.
2. Genera o recupera `device_id` desde NVS.
3. Inicia OLED y BLE provisioning.
4. Recibe `ssid`, `password`, `user_id` y parametros AWS por BLE.
5. Conecta a Wi-Fi y arranca MQTT sobre AWS IoT Core.
6. Publica `claim` al asociar el dispositivo y `heartbeat` periodico cuando MQTT esta activo.
7. Permite reprovisioning con boton fisico y muestra estado local en OLED.

## Topics base
- `lab/devices/<device_id>/claim`
- `lab/devices/<device_id>/heartbeat`
- `lab/devices/<device_id>/sensor_registry`
- `lab/devices/<device_id>/telemetry`

## Payload base de claim
```json
{
  "device_id": "lab_XXXXXX",
  "user_id": "uuid-del-usuario",
  "device_kind": "climate_sensor"
}
```

## Payload base de sensor registry
```json
{
  "device_id": "lab_XXXXXX",
  "sensor_key": "ambient_1",
  "sensor_type": "sht45",
  "vendor": "adafruit",
  "model": "SHT45",
  "measures": ["temperature_c", "humidity_rh", "vpd_kpa"],
  "unit_map": {
    "temperature_c": "C",
    "humidity_rh": "%",
    "vpd_kpa": "kPa"
  },
  "i2c_address": "0x44",
  "bus": "i2c0",
  "position": "canopy",
  "is_active": true,
  "metadata": {
    "source": "firmware"
  }
}
```

## Payload base de telemetry
```json
{
  "device_id": "lab_XXXXXX",
  "sensor_key": "ambient_1",
  "temperature_c": 25.4,
  "humidity_rh": 68.2,
  "vpd_kpa": 1.03,
  "uptime_ms": 123456
}
```

## Archivos clave
- `src/main.cpp`: orquestacion general, Wi-Fi, BLE, AWS y watchdogs.
- `src/sensor_registry.cpp`: construccion del payload JSON para registrar sensores.
- `src/sht45_sensor.cpp`: lectura real del SHT45, calculo de VPD y payload de telemetria.
- `src/provisioning.cpp`: servicio BLE GATT y parseo de credenciales.
- `src/oled_display.cpp`: estado visual local.
- `src/Config.cpp`: wrapper de NVS.
- `platformio.ini`: board, puertos, SPIFFS y flags de compilacion.

## Certificados y despliegue
- Coloca en `data/certs/`:
  - `AmazonRootCA1.pem`
  - `device.pem.crt`
  - `private.pem.key`
- Sube SPIFFS con `pio run --target uploadfs`.
- Flashea firmware con `pio run --target upload`.
- Monitorea serial con `pio device monitor`.

## Siguientes extensiones
Este repositorio queda listo para agregar modulos de negocio encima de la base, por ejemplo:
- telemetria de sensores
- comandos remotos por MQTT
- integracion backend adicional
- OTA
