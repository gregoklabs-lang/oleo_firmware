# Firmware ESP32 - Integracion BLE + AWS IoT

## Panorama general
Firmware para ESP32-C3 (board `adafruit_qtpy_esp32c3`) que gestiona la provision BLE, la conexion Wi-Fi y la comunicacion MQTT segura con AWS IoT Core. El dispositivo se integra con:
- Lambda como router de mensajes hacia Supabase.
- Supabase para persistencia, reglas RLS y vistas analiticas.
- App Flutter que refleja estados en tiempo real mediante topics MQTT.

## Arquitectura de punta a punta
- **Dispositivo**: ESP32-C3 con BLE de aprovisionamiento, interfaz OLED y conectividad Wi-Fi.
- **AWS IoT Core**: Endpoint `a7xxu98k219gv-ats.iot.us-east-1.amazonaws.com` con autenticacion mutua (Root CA + certificado + llave privada).
- **AWS Lambda**: Normaliza y enruta mensajes entrantes desde `devices/<device_id>/claim` hacia Supabase.
- **Supabase**: Guarda claims y estados del dispositivo, expone vistas protegidas por RLS para la app.
- **App Flutter**: Gestiona onboarding del usuario, alimenta credenciales al dispositivo via BLE y presenta estados en vivo.

## Flujo operativo
1. **Boot y SPIFFS**: Monta SPIFFS y, en caso de error, intenta formatear y volver a montar. Emite logs por Serial.
2. **Identidad**: Calcula `device_id` (prefijo `OLEO_` + sufijo de MAC), lo guarda en NVS y agenda log diferido con `user_id` y direccion MAC.
3. **Display**: Inicializa U8g2 sobre I2C con un icono circular que refleja conectividad (relleno, contorno o parpadeo).
4. **Provisioning BLE**: Servicio y caracteristica personalizadas aceptan payload `SSID|password|user_id` o con saltos de linea. La cola FreeRTOS desacopla callbacks y evita trabajo pesado en interrupciones.
5. **Boton fisico**: GPIO0 con interrupcion (debounce + presion prolongada de 3 s) que borra credenciales Wi-Fi y activa sesion BLE de 60 s.
6. **Conexion Wi-Fi**: Se conecta con credenciales existentes o espera nuevas via BLE. Publica estados a la app (`wifi:conectando`, `wifi:conectado`, `wifi:error`).
7. **AWS IoT Core**: Tras asociarse a la red Wi-Fi, carga certificados desde SPIFFS y establece cliente MQTT TLS (buffer 1 KB, reintento cada 5 s). Publica claim JSON en `devices/<device_id>/claim`.
8. **Loop principal**: Procesa boton, expiracion BLE, estado Wi-Fi, reconexiones AWS, cola de eventos y refresco OLED con `delay(1)` para ceder CPU.

## Componentes clave del firmware
- **src/main.cpp**: Orquesta estados globales, manejo de Wi-Fi/BLE/AWS, logs con prefijo `device_id` y scheduler de identidad.
- **src/provisioning.cpp / provisioning.h**: Encapsula BLE GATT, parseo defensivo de credenciales (trim, limites de longitud) y notificaciones reactivas.
- **src/oled_display.cpp / oled_display.h**: Abstraccion para U8g2, gestiona parpadeo cuando BLE esta activo.
- **data/certs/**: Certificados PEM que se cargan en SPIFFS (`AmazonRootCA1.pem`, `device.pem.crt`, `private.pem.key`).
- **platformio.ini**: Define entorno `adafruit_qtpy_esp32c3`, monitor serie 9600, particiones `huge_app.csv` y dependencias (PubSubClient, U8g2, ArduinoJson).

## Configuracion y despliegue
- Copiar certificados actualizados a `data/certs` y ejecutar `pio run --target uploadfs` para montarlos en SPIFFS.
- Ajustar `AWS_IOT_ENDPOINT`, topics y puertos segun ambiente (dev, staging, prod) antes del build.
- Programar el binario con `pio run --target upload` y validar logs en el monitor serie (9600 bps).
- Verificar que la app Flutter detecte el anuncio BLE (nombre `device_id`) y pueda enviar credenciales.

## Observabilidad y soporte
- Logs serializados mediante `logWithDeviceId` para facilitar trazabilidad multi-dispositivo.
- Notificaciones BLE oportunas para la app durante provisioning y fallas.
- OLED como indicador local para personal de campo sin necesidad de consola.

## Puntos de mejora para produccion comercial
- **Seguridad de certificados**: SPIFFS sin cifrado; evaluar NVS cifrado, secure element o PKCS#11 con rotacion remota.
- **Resiliencia MQTT**: Agregar backoff exponencial real, watchdog de reconexion, QoS1/QoS2 segun SLA y suscripciones de control (comandos remotos).
- **Provisionamiento seguro**: Evaluar emparejamiento BLE o token de un solo uso para evitar aprovisionamientos no autorizados.
- **Gestion Wi-Fi avanzada**: Escaneo activo, memoria de redes priorizadas, reintentos graduados y actualizacion remota de credenciales desde backend.
- **Telemetria ampliada**: Enviar eventos de estado, metricas de uptime y errores a topics secundarios para monitoreo.
- **Estrategia OTA**: Integrar actualizaciones via AWS IoT Jobs u otra solucion robusta con rollback.
- **UX de campo**: Mensajes textuales breves en la OLED ante fallas y modo ahorro de energia cuando no hay actividad.
- **Testing automatizado**: Crear pruebas unitarias para parsing de credenciales, colas BLE y reconexion MQTT; considerar CI con simulaciones de Wi-Fi.
- **Configuracion parametrica**: Externalizar constantes (endpoints, topics, timers) en archivo de configuracion o particion NVS editable.

## Checklist sugerido antes de liberar
- Claim recibido en Lambda y reflejado en Supabase con `user_id` correcto.
- BLE se desactiva tras 60 s o al conectar Wi-Fi, sin fugas de memoria.
- MQTT se reconecta tras perdida de Wi-Fi en menos de 30 s.
- OLED refleja estados correctos durante provisioning y fallas inducidas.
- Pruebas de presion larga del boton con rebotes controlados en hardware objetivo.
