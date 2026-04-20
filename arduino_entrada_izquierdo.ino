/*
 * Smart Portium — Arduino UNO #1
 * PORTÓN ENTRADA (Izquierdo) — MOT-100
 *
 * Conexión ESP32:
 *   Pin 10 (RX) ← ESP32 GPIO17 (TX)
 *   Pin 11 (TX) → ESP32 GPIO16 (RX)
 *
 * Microswitches:
 *   Pin 2 = SW_CERRADO (INPUT_PULLUP, NO)
 *   Pin 3 = SW_ABIERTO  (INPUT_PULLUP, NO)
 *
 * Motor MOT-100 (izquierdo):
 *   write(180) = abrir
 *   write(0)   = cerrar
 *   write(90)  = detener
 */

#include <SoftwareSerial.h>
#include <Servo.h>

SoftwareSerial espSerial(10, 11); // RX, TX
Servo motor;

#define PIN_MOTOR      9
#define PIN_SW_CERRADO 2
#define PIN_SW_ABIERTO 3

#define TIMEOUT_MOTOR  15000  // 15s máximo de movimiento
#define TIEMPO_ABIERTO 25000  // 25s antes de auto-cerrar

// Estados
#define CERRADO  0
#define ABRIENDO 1
#define ABIERTO  2
#define CERRANDO 3

int estado = CERRADO;
unsigned long tiempoAbierto = 0;

bool swCerradoActivo() { return digitalRead(PIN_SW_CERRADO) == LOW; }
bool swAbiertoActivo()  { return digitalRead(PIN_SW_ABIERTO)  == LOW; }

void motorAbrir()   { motor.write(180); }
void motorCerrar()  { motor.write(0);   }
void motorDetener() { motor.write(90);  }

void enviarStatus(const char* s) {
  for (int i = 0; i < 3; i++) {
    espSerial.print("STATUS:");
    espSerial.println(s);
    delay(50);
  }
}

void abrir() {
  if (estado == ABIERTO || estado == ABRIENDO) return;
  estado = ABRIENDO;
  enviarStatus("OPENING");
  motorAbrir();
  unsigned long t = millis();
  while (!swAbiertoActivo()) {
    if (millis() - t > TIMEOUT_MOTOR) break;
    delay(10);
  }
  motorDetener();
  estado = ABIERTO;
  tiempoAbierto = millis();
  enviarStatus("OPEN");
}

void cerrar() {
  if (estado == CERRADO || estado == CERRANDO) return;
  estado = CERRANDO;
  enviarStatus("CLOSING");
  motorCerrar();
  unsigned long t = millis();
  while (!swCerradoActivo()) {
    if (millis() - t > TIMEOUT_MOTOR) break;
    delay(10);
  }
  motorDetener();
  estado = CERRADO;
  enviarStatus("CLOSED");
}

void leerComando() {
  if (!espSerial.available()) return;
  String cmd = espSerial.readStringUntil('\n');
  cmd.trim();
  if      (cmd == "OPEN")   abrir();
  else if (cmd == "CLOSE")  cerrar();
  else if (cmd == "STATUS") {
    if      (estado == CERRADO)  enviarStatus("CLOSED");
    else if (estado == ABIERTO)  enviarStatus("OPEN");
    else if (estado == ABRIENDO) enviarStatus("OPENING");
    else if (estado == CERRANDO) enviarStatus("CLOSING");
  }
}

void setup() {
  Serial.begin(9600);
  espSerial.begin(9600);
  motor.attach(PIN_MOTOR);
  motorDetener();
  pinMode(PIN_SW_CERRADO, INPUT_PULLUP);
  pinMode(PIN_SW_ABIERTO, INPUT_PULLUP);
  // Estado inicial
  if (swCerradoActivo()) estado = CERRADO;
  else if (swAbiertoActivo()) estado = ABIERTO;
  else estado = CERRADO;
  Serial.println("Arduino Entrada (Izquierdo) listo");
}

void loop() {
  leerComando();
  // Auto-cerrar después de TIEMPO_ABIERTO
  if (estado == ABIERTO && millis() - tiempoAbierto > TIEMPO_ABIERTO) {
    Serial.println("Auto-cerrando...");
    cerrar();
  }
}
