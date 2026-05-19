#include <Wire.h>
#include <SPI.h>
#include <math.h>
#include <DHT.h>
#include <DHT_U.h>
#include "Adafruit_MAX31855.h"
#include <SoftwareSerial.h>

SoftwareSerial espSerial(2, 3);  // RX, TX
// 🔥 buffer de recepción
String buffer = "";

// ================== CONFIG ==================
#define BAUD 9600

float factor_tiempo = 1;

// ---------- TIEMPO ----------
float dt_real = 1;
float dt;

bool ledActivo = false;
unsigned long ledTimer = 0;

bool cfgRecibido = false;

float serialSimulado = 15;  //Periodo de muestreo para base de datos

unsigned long intervaloSensor;
unsigned long intervaloControl;
unsigned long intervaloSerial;

float calcular_dt_real(float factor) {

  if (factor <= 1) return 1.0;     //24h
  if (factor <= 2) return 0.5;     //12h
  if (factor <= 4) return 0.25;    //6h
  if (factor <= 8) return 0.125;   //3h
  if (factor <= 48) return 0.1;    //30 min
  if (factor <= 120) return 0.05;  //12 min
  if (factor <= 300) return 0.02;  //4.8 min

  return 0.01;  //1 min
}

// ---------- PARÁMETROS ----------
float tau_int = 80.0;
float tau_c12 = 120.0;
float tau_delta = 95.0;

// HUMEDAD
float tau_up = 300.0;
float tau_down = 40.0;

// ---------- ESTADOS ----------
float c12;
float c1 = 0, c2 = 0;
float delta_int_c12_real;
float I = 0;
float puntoRocio = 0, error = 0, pwm = 0, pwmAnterior = 0;

float hInt = 73.3;  // condición inicial experimental
float tInt;
float hExt;
float tExt;

bool okInt = true, okExt = true;

// ---------- CONTROL ----------
unsigned long TSensor = 0, TControl = 0, TSerial = 0;

float kp = 1.3, ki = 0.05;
float errorAcumulado = 0.0;
unsigned long tiempoAnterior = 0;
const float maxIntegracion = 100.0;

bool modoManual = false;
int pwmManual = 0;

int Vel_PWM_A = 0, Vel_PWM_B = 0;

int estadoSistema = 0;
// 0 = OK
// 1 = Sobrecorriente
// 2 = Sobretemperatura
// 3 = Error sensores
// 4 = Humedad baja
int estadoControl = 0;  // 0 reposo, 1 ejecución

float humedadObjetivo = 0;
bool pausaHumedad = false;

// =========================
// PROMEDIOS
// =========================
float sumaCorriente = 0;
float sumaPWM = 0;

float suma_tExt = 0;
float suma_tInt = 0;
float suma_hExt = 0;
float suma_hInt = 0;
float suma_c12 = 0;
float suma_c1 = 0;
float suma_c2 = 0;
float sumaRocio = 0;
float sumaError = 0;

int contadorMuestras = 0;

// promedios finales
float corrienteProm = 0;
float pwmProm = 0;

float tExtProm = 0;
float tIntProm = 0;
float hExtProm = 0;
float hIntProm = 0;
float c12Prom = 0;
float c1Prom = 0;
float c2Prom = 0;
float rocProm = 0;
float errorProm = 0;


// ---------- VENTILADOR ----------
float ventilador_real(int pwm) {
  int pwm_dead = 48;
  if (pwm <= pwm_dead) return 0.0;
  return pow((pwm - pwm_dead) / float(255 - pwm_dead), 0.6);
}


// =========================
// FUNCIONES AUXILIARES
// =========================
float calcularPuntoRocio(float tempC, float humRel) {
  float a = 17.62, b = 243.12;
  float gamma = log(humRel / 100.0) + (a * tempC) / (b + tempC);
  return ((b * gamma) / (a - gamma));
}

float leerCorrienteFiltrada() {
  return pwm * 0.03;
}

unsigned long calcularTSensor(float factor) {

  unsigned long t = 1000 / factor;

  if (t < 50) t = 50;

  return t;
}

unsigned long calcularTControl(float factor) {

  unsigned long t = 5000 / factor;

  if (t < 100) t = 100;

  return t;
}

unsigned long calcularTSerial(float factor) {

  // serialSimulado está en segundos simulados
  unsigned long t = (serialSimulado * 1000.0) / factor;

  if (t < 500) t = 500;

  return t;
}

// ---------- tExt ----------
float tExt_simulada() {

  float t_min = 27.4;
  float t_max = 30.8;

  float media = (t_max + t_min) / 2.0;
  float amp = (t_max - t_min) / 2.0;

  float periodo = 86400.0;
  float omega = 2 * PI / periodo;

  float tiempo = (millis() / 1000.0) * factor_tiempo;

  float ruido = random(-10, 10) / 1000.0;

  return media + amp * sin(omega * tiempo - PI / 2) + ruido;
}

// =========================
// SETUP
// =========================
void setup() {
  Serial.begin(BAUD);
  espSerial.begin(BAUD);
  //Serial.println("=== Arduino listo ===");
  randomSeed(analogRead(0));

  dt_real = calcular_dt_real(factor_tiempo);
  dt = dt_real * factor_tiempo;

  float factor_pwm = 0.0;

  float delta_base = 5.9 + (21.25 - 5.9) * factor_pwm;
  float delta_target = 3.9 + (16.55 - 3.9) * factor_pwm;

  tExt = tExt_simulada();

  intervaloSensor = calcularTSensor(factor_tiempo);
  intervaloControl = calcularTControl(factor_tiempo);
  intervaloSerial = calcularTSerial(factor_tiempo);

  // temperatura inicial
  c12 = tExt - delta_base;
  delta_int_c12_real = delta_target;
  tInt = c12 + delta_int_c12_real;

  pinMode(13, OUTPUT);
  digitalWrite(13, LOW);

  tiempoAnterior = millis();
}

// =========================
// LOOP
// =========================
void loop() {

  unsigned long TA = millis();
  // =========================
  // APAGAR LED
  // =========================
  if (ledActivo && millis() - ledTimer > 200) {

    digitalWrite(13, LOW);
    ledActivo = false;
  }

  // =========================
  // RECEPCIÓN SERIAL NO BLOQUEANTE
  // =========================
  while (espSerial.available()) {

    char c = espSerial.read();

    if (c == '\n') {

      buffer.trim();

      if (buffer.startsWith("CFG,")) {

        //Serial.println(buffer);

        String datos = buffer.substring(4);

        int idx[8];
        int start = 0;

        for (int i = 0; i < 8; i++) {
          idx[i] = datos.indexOf(',', start);
          start = idx[i] + 1;
        }

        // =========================
        // VALIDAR TRAMA
        // =========================
        bool valido = true;

        for (int i = 0; i < 8; i++) {

          if (idx[i] == -1) {
            valido = false;
          }
        }

        if (!valido) {

          buffer = "";
          break;
        }

        modoManual = datos.substring(
                            0,
                            idx[0])
                       .toInt();

        pwmManual = datos.substring(
                           idx[0] + 1,
                           idx[1])
                      .toInt();

        kp = datos.substring(
                    idx[1] + 1,
                    idx[2])
               .toInt()
             / 100.0;

        ki = datos.substring(
                    idx[2] + 1,
                    idx[3])
               .toInt()
             / 100.0;

        Vel_PWM_A = datos.substring(
                           idx[3] + 1,
                           idx[4])
                      .toInt();

        Vel_PWM_B = datos.substring(
                           idx[4] + 1,
                           idx[5])
                      .toInt();

        humedadObjetivo = datos.substring(
                                 idx[5] + 1,
                                 idx[6])
                            .toFloat();

        serialSimulado = datos.substring(
                                idx[6] + 1,
                                idx[7])
                           .toFloat();

        estadoControl = datos.substring(
                               idx[7] + 1)
                          .toInt();

        if (estadoControl == 0) {

          pwm = 0;
          pwmManual = 0;

          Vel_PWM_A = 0;
          Vel_PWM_B = 0;

          errorAcumulado = 0;

          pausaHumedad = false;
          humedadObjetivo = 0;
        }

        cfgRecibido = true;
        digitalWrite(13, HIGH);
        espSerial.println("ACK");


        ledActivo = true;
        ledTimer = millis();

        intervaloSerial = calcularTSerial(factor_tiempo);

        errorAcumulado = 0;
      }

      // limpiar buffer
      buffer = "";
    }

    else {
      buffer += c;
      if (buffer.length() > 80) buffer = "";
    }
  }

  // =========================
  // LECTURA SENSORES
  // =========================
  if (TA - TSensor >= intervaloSensor) {
    TSensor = TA;

    tExt = tExt_simulada();

    // 🔥 SOLO UNA VEZ (evita error)
    float factor_pwm = pwm / 255.0;
    float factor_vent = ventilador_real(Vel_PWM_A);

    float delta_base = 5.9 + (21.25 - 5.9) * factor_pwm;
    float delta_target = 3.9 + (16.55 - 3.9) * factor_pwm;

    // ---------- c12 ----------
    float c12_base = tExt - delta_base;
    float incremento = 1.5 * factor_vent;
    float c12_obj = c12_base + incremento;

    c12 += (c12_obj - c12) * (dt / tau_c12);

    float ruido1 = random(-10, 10) / 100.0;
    float ruido2 = random(-10, 10) / 100.0;

    c1 = c12 + ruido1;
    c2 = c12 + ruido2;

    // ---------- tInt ----------
    delta_int_c12_real += (delta_target - delta_int_c12_real) * (dt / tau_delta);

    float mix_air = 0.6 * factor_vent;
    float t_base = c12 + delta_int_c12_real;
    float t_obj = (1 - mix_air) * t_base + mix_air * tExt;

    tInt += (t_obj - tInt) * (dt / tau_int);

    if (tInt > tExt) tInt = tExt;
    if (tInt < c12) tInt = c12;

    // ================= HUMEDAD (MODELO EXPERIMENTAL) =================

    float media = (27.4 + 30.8) / 2.0;
    float omega = 2 * PI / 86400.0;
    float tiempo = (millis() / 1000.0) * factor_tiempo;

    float fase_h = 2.7;

    // hExt con amplitud ≈ 18
    hExt = 56.8 + 9.0 * sin(omega * tiempo + fase_h);

    // ---------- OFFSET ----------
    float offset_base = 16.5;
    float offset_pwm = offset_base - 3.0 * factor_pwm;

    // 🔥 mezcla realista
    float mix = 0.7 * factor_vent;

    // 🔥 offset mínimo (clave)
    float offset_min = 5.0;

    // 🔥 offset final
    float offset = (1 - mix) * offset_pwm + mix * offset_min;

    float h_obj = hExt + offset;

    // ---------- DINÁMICA ----------
    float tau;

    if (h_obj < hInt) {
      tau = tau_down;
    } else {
      tau = tau_up;
    }

    hInt += (h_obj - hInt) * (dt / tau);

    // límites
    if (hInt < 0) hInt = 0;
    if (hInt > 100) hInt = 100;

    puntoRocio = calcularPuntoRocio(tExt, hExt);
    I = leerCorrienteFiltrada();  // Corriente de las dos celdas Peltier

    if (isnan(c1) || isnan(c2) || !okInt || !okExt) estadoSistema = 3;
    else if (c12 > 80.0) estadoSistema = 2;
    else if (I > 15.0) estadoSistema = 1;
    else estadoSistema = 0;

    // PAUSA POR HUMEDAD (SOLO SI TODO OK)
    if (estadoSistema == 0 && estadoControl == 1 && !isnan(hExt)) {

      if (hExt < humedadObjetivo - 2) {
        pausaHumedad = true;
      } else if (hExt > humedadObjetivo + 2) {
        pausaHumedad = false;
      }

      if (pausaHumedad) {
        estadoSistema = 4;
      }
    }



    // =========================
    // ACUMULAR PROMEDIOS
    // =========================
    sumaCorriente += I;
    pwm = constrain(pwm, 0, 255);
    sumaPWM += pwm;

    suma_tExt += tExt;
    suma_tInt += tInt;
    suma_hExt += hExt;
    suma_hInt += hInt;
    suma_c12 += c12;
    suma_c1 += c1;
    suma_c2 += c2;
    sumaRocio += puntoRocio;

    if (estadoControl == 1 && !modoManual) {
      sumaError += error;
    }

    contadorMuestras++;
  }

  // =========================
  // CONTROL
  // =========================
  if (TA - TControl >= intervaloControl) {
    TControl = TA;

    // =========================
    // REPOSO (PRIORIDAD MÁXIMA)
    // =========================
    if (estadoControl == 0) {

      pwm = 0;
      Vel_PWM_A = 0;
      Vel_PWM_B = 0;

      errorAcumulado = 0;

      pausaHumedad = false;
      estadoSistema = 0;

      humedadObjetivo = 0;
      pwmProm = 0;
      corrienteProm = 0;
      sumaPWM = 0;
      sumaCorriente = 0;
    }

    // =========================
    // PAUSA HUMEDAD
    // =========================
    else if (pausaHumedad) {

      pwm = 0;
      Vel_PWM_A = 0;
      Vel_PWM_B = 0;
      errorAcumulado = 0;
    }

    // =========================
    // CONTROL NORMAL
    // =========================
    else {

      bool sobrecorriente = (I > 15.0);

      if (sobrecorriente) {

        pwm = 0;
        Vel_PWM_A = 0;
        Vel_PWM_B = 0;
        errorAcumulado = 0;
      }

      // =========================
      // CONTROL AUTOMÁTICO
      // =========================
      if (!modoManual && estadoSistema == 0 && !sobrecorriente) {

        if (!isnan(c12) && !isnan(tExt) && !isnan(hExt)) {

          error = puntoRocio - c12;

          float dt_control = ((millis() - tiempoAnterior) / 1000.0) * factor_tiempo;
          tiempoAnterior = millis();

          if (pwm > 0 && pwm < 255) {
            errorAcumulado += error * dt_control;
          }

          errorAcumulado = constrain(errorAcumulado, -maxIntegracion, maxIntegracion);

          float salidaPI = kp * error + ki * errorAcumulado;
          float tempObjetivo = salidaPI + c12;

          pwm = (tempObjetivo - 28.0) / -0.0745;
          pwm = constrain(pwm, 0, 255);
        } else {
          pwm = 0;
          errorAcumulado = 0;
        }

        if (abs(pwm - pwmAnterior) > 2) {
          pwmAnterior = pwm;
        }

      }
      // =========================
      // MODO MANUAL
      // =========================
      else if (modoManual && !sobrecorriente) {

        pwm = constrain(pwmManual, 0, 255);  // límite seguro
        pwmAnterior = pwm;
      }
    }
  }


  // =========================
  // ENVÍO SERIAL
  // =========================
  // =========================
  // ENVÍO SERIAL (ESCALADO x100 SIN VARIABLES EXTRA)
  // =========================
  if (TA - TSerial >= intervaloSerial) {
    TSerial = TA;

    // =========================
    // CALCULAR PROMEDIOS
    // =========================
    if (contadorMuestras > 0) {

      corrienteProm = sumaCorriente / contadorMuestras;
      pwmProm = sumaPWM / contadorMuestras;

      tExtProm = suma_tExt / contadorMuestras;
      tIntProm = suma_tInt / contadorMuestras;
      hExtProm = suma_hExt / contadorMuestras;
      hIntProm = suma_hInt / contadorMuestras;
      c12Prom = suma_c12 / contadorMuestras;
      c1Prom = suma_c1 / contadorMuestras;
      c2Prom = suma_c2 / contadorMuestras;
      rocProm = sumaRocio / contadorMuestras;
      errorProm = sumaError / contadorMuestras;
    }

    espSerial.print((long)(tExtProm * 100));
    espSerial.print(",");
    espSerial.print((long)(hExtProm * 100));
    espSerial.print(",");
    espSerial.print((long)(tIntProm * 100));
    espSerial.print(",");
    espSerial.print((long)(hIntProm * 100));
    espSerial.print(",");
    espSerial.print((long)(c1Prom * 100));
    espSerial.print(",");
    espSerial.print((long)(c2Prom * 100));
    espSerial.print(",");
    espSerial.print((long)(c12Prom * 100));
    espSerial.print(",");
    espSerial.print((long)(rocProm * 100));
    espSerial.print(",");
    espSerial.print((long)(errorProm * 100));
    espSerial.print(",");
    espSerial.print((int)(pwmProm));
    espSerial.print(",");
    espSerial.print(Vel_PWM_A);
    espSerial.print(",");
    espSerial.print(Vel_PWM_B);
    espSerial.print(",");
    espSerial.print((long)(corrienteProm * 100));
    espSerial.print(",");
    espSerial.println(estadoSistema);

    // =========================
    // RESET PROMEDIOS
    // =========================
    sumaCorriente = 0;
    sumaPWM = 0;

    suma_tExt = 0;
    suma_tInt = 0;
    suma_hExt = 0;
    suma_hInt = 0;
    suma_c12 = 0;
    suma_c1 = 0;
    suma_c2 = 0;
    sumaRocio = 0;
    sumaError = 0;

    contadorMuestras = 0;
  }
}