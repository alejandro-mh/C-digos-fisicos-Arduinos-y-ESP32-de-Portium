Este repositorio contiene el código fuente para el control de hardware del sistema de portón inteligente. El proyecto utiliza una arquitectura de comunicación híbrida entre un **ESP32** (como puerta de enlace IoT) y dos **Arduino UNO** (como controladores de actuadores físicos).

##  Descripción del Sistema

El sistema permite la apertura y cierre de un portón doble mediante comandos enviados vía **MQTT** a través de WebSockets. 

- **ESP32:** Actúa como el núcleo de comunicaciones. Gestiona el portal de aprovisionamiento Wi-Fi, la conexión segura con el broker MQTT (vía Cloudflare Tunnel) y la distribución de comandos mediante comunicación serial.
- **Arduino UNO (x2):** Encargados del control de bajo nivel. Operan los motores mediante relevadores y monitorean los sensores de fin de carrera para garantizar un movimiento preciso y seguro.

## Estructura del Repositorio

* `/ESP32_Gateway/`: Código principal para el ESP32. Incluye la lógica de conexión, enmascaramiento de WebSockets y parseo de JSON.
* `/Arduino_Izquierdo/`: Firmware para el controlador del motor del lado izquierdo.
* `/Arduino_Derecho/`: Firmware para el controlador del motor del lado derecho (giro invertido).

## Tecnologías y Protocolos

* **Lenguaje:** C++ (Arduino Framework)
* **Protocolo de Red:** MQTT sobre WebSockets (HTTP/1.1 Handshake manual).
* **Comunicación Interna:** Serial UART (ESP32 <-> Arduinos).
* **Gestión de Datos:** ArduinoJson para el procesamiento de estados y comandos.

## Configuración Rápida

1.  Cargar el código en los respectivos microcontroladores.
2.  Al encender el ESP32 por primera vez, este iniciará en modo **Access Point** si no detecta credenciales.
3.  Conectarse a la red generada para configurar el SSID y el Token de registro del sistema.
4.  Una vez configurado, el dispositivo se vinculará automáticamente al backend de `victech.mx`.

---
Desarrollado como parte del proyecto de seguridad física.
