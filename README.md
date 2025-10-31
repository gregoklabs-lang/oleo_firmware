# Firmware ESP32 – Integración BLE + AWS IoT

## Resumen Ejecutivo
Firmware para ESP32-C3 (board `adafruit_qtpy_esp32c3`) que gestiona provisión BLE, conexión Wi-Fi y publicación MQTT segura hacia AWS IoT Core. Se coordina con:
- Lambda como router de mensajes hacia Supabase.
- Supabase (persistencia, RLS, vistas).
- App Flutter con vistas en tiempo real (topics reclamados y estados).

## Stack Base
- **Framework**: Arduino (PlatformIO).
- **Librerías clave**: `WiFi`, `BLEDevice`, `PubSubClient`, `U8g2` (OLED), `Preferences`, `SPIFFS`.
- **Certificados**: almacenados en `data/certs`, desplegados en SPIFFS (`AmazonRootCA1.pem`, `device.pem.crt`, `private.pem.key`).
- **Particiones**: `huge_app.csv` para disponer de memoria de programa amplia.

## Flujo General de Ciclo de Vida
1. **Boot & SPIFFS**: Monta SPIFFS, intenta formatear si falla; registra eventos por serial.
2. **Identidad**: Genera `device_id` derivado de MAC (`OLEO_XXXXXX`), persiste en NVS y programa log diferido de identidad (MAC y `user_id`).
3. **Display**: Inicializa OLED U8g2; representa conectividad Wi-Fi o BLE activo mediante icono circular y parpadeo.
4. **Provisioning BLE**: Prepara servicio BLE custom y característica RW/notify; en reposo anuncia “inactivo”.
5. **Conexión Wi-Fi**: Si hay credenciales almacenadas usa `WiFi.begin`; si fallan o no existen, queda a la espera de BLE.
6. **AWS IoT Core**: Tras Wi-Fi conecta MQTT TLS mutual auth; publica reclamo (`devices/<device_id>/claim`) con `device_id` y `user_id`.
7. **Loop Principal**: Verifica botón, timeouts BLE, estado Wi-Fi, reconexión AWS, cola de eventos BLE y refresco OLED.

## Provisión BLE
- **Servicio/Característica** UUID fijos (sin emparejamiento).
- **Entrada**: Cadena `SSID|password|user_id` (o saltos de línea). Valida vacíos y longitudes; si hay error envía `error:<causa>`.
- **Cola**: Usa `FreeRTOS` queue para desacoplar eventos y mantener ISR ligeros.
- **Notificaciones**: `inactivo`, `activo`, `credenciales`, `wifi:conectando`, `wifi:conectado`, `wifi:error`, etc.
- **Activación**: Botón físico (GPIO0) con debounce + detección de presión larga (3 s) que borra credenciales previas y reinicia sesión BLE de 60 s.

## Conectividad Wi-Fi y AWS
- **Reintento Wi-Fi**: seguimiento de temporizador (`kWifiConnectTimeoutMs=15s`), reporta a BLE en caso de error.
- **Reconexión**: `WiFi.setAutoReconnect(true)` y `mqttClient.loop()` en main loop. Retraso entre intentos MQTT (`kAwsReconnectDelayMs=5s`).
- **Claim**: `mqttClient.publish` a topic `devices/<device_id>/claim`. La Lambda receptora infiere `user_id` y sincroniza con Supabase.
- **Persistencia usuario**: `Preferences` (namespace `identity`) guarda `user_id` recibido vía BLE.

## Estados Visuales
- **Wi-Fi estable**: círculo sólido.
- **BLE activo**: círculo parpadeando (600 ms).
- **Sin conexión**: solo contorno.

## Diagnóstico y Observabilidad
- **Serial**: `logWithDeviceId` antepone `device_id` a los mensajes.
- **Eventos**: registra `BLE`, `WIFI`, `AWS`, `IDENTIDAD`, `SPIFFS`.
- **Temporización**: `delay(1)` en loop para ceder CPU.

## Puntos de Mejora (Producción Comercial)
- **Seguridad Certificados**: SPIFFS sin cifrado; considerar NVS cifrado, secure element o PKCS#11/HSM junto con política de rotación automatizada.
- **Resiliencia MQTT**: Falta suscripción/ACK inbound, heartbeats explícitos, backoff exponencial y watchdog de conectividad; agregar QoS1/2 según SLA.
- **Provisioning BLE**: Añadir pairing/bonding o autenticación out-of-band; proteger frente a provisiones maliciosas y registrar intentos fallidos.
- **Gestión Wi-Fi**: Implementar escaneo activo, prioridad de redes y política de reintentos escalonados; contemplar actualización dinámica de credenciales vía backend.
- **Logs & Telemetría**: Integrar niveles de log, buffering y envío a backend (por ejemplo topic `events/`), más métricas de rendimiento/errores.
- **Mantenimiento**: Incluir estrategia OTA (esp_http_client / AWS Jobs) y control de versiones del firmware.
- **Botón físico**: Definir fallback para rebotes extremos y notificar cancelaciones si se suelta antes del umbral.
- **OLED UX**: Añadir mensajes textuales breves sobre fallos (en lugar de solo iconos) y modo ahorro energía con dimming.
- **Testing**: Incorporar pruebas unitarias/mocks para parsing de credenciales, colas BLE y reconexiones AWS, además de tests de estrés BLE concurrente.
- **Configuración**: Externalizar endpoint y topics a `config` en NVS/SPIFFS editable para distintas etapas (dev/staging/prod); automatizar carga de certificados por entorno.
