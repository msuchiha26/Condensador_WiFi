import os
import psycopg2
from psycopg2 import pool
import time
from flask import Flask, request, jsonify, render_template, Response

app = Flask(__name__)

DATABASE_URL = os.getenv("DATABASE_URL")

# =========================
# POOL
# =========================
db_pool = None

def get_conn():
    global db_pool

    if db_pool is None:
        db_pool = pool.SimpleConnectionPool(
            1, 10,
            dsn=DATABASE_URL,
            sslmode='require'
        )

    return db_pool.getconn()

def release_conn(conn):
    global db_pool
    if db_pool:
        db_pool.putconn(conn)

# =========================
# VARIABLE GLOBAL (LIVE)
# =========================
latest_data = {
    "data": {},
    "last_seen": 0
}
# =========================
# HOME
# =========================
@app.route('/')
def home():
    return "API funcionando 🚀"

# =========================
# DASHBOARD
# =========================
@app.route('/dashboard')
def dashboard():
    return render_template('index.html')

# =========================
# LIVE DATA
# =========================
@app.route('/live')
def live():
    return jsonify(latest_data.get("data", {}))

# =========================
# CONFIG ACTUAL
# =========================
@app.route('/config', methods=['GET'])
def get_config():
    conn = get_conn()
    cur = conn.cursor()

    try:
        # 🔍 estado actual
        cur.execute("SELECT estado_control FROM config_actual WHERE id=1")
        estado = cur.fetchone()[0]

        if estado == 1:
            # 🔴 EXPERIMENTO (snapshot congelado)
            cur.execute("""
                SELECT 1 AS estado_control, modoManual, pwm, kp, ki, velA, velB, humedad_objetivo, tserial,
                       (SELECT descargado FROM experimentos ORDER BY id DESC LIMIT 1)
                FROM experimentos
                WHERE fecha_fin IS NULL
                ORDER BY id DESC LIMIT 1
            """)
        else:
            # 🟢 REPOSO (config editable)
            cur.execute("""
                SELECT estado_control, modoManual, pwm, kp, ki, velA, velB, humedad_objetivo, tserial,
                       (SELECT descargado FROM experimentos ORDER BY id DESC LIMIT 1)
                FROM config_actual
                WHERE id = 1
            """)

        row = cur.fetchone()

        return jsonify({
            "estado_control": row[0],
            "modoManual": row[1],
            "pwm": row[2],
            "kp": row[3],
            "ki": row[4],
            "velA": row[5],
            "velB": row[6],
            "humedad_objetivo": row[7],  # 🔥 NUEVO
            "tserial": row[8],
            "descargado": row[9] if row[9] is not None else False
        })

    except Exception as e:
        conn.rollback()
        print("ERROR CONFIG GET:", e)
        return {"error": str(e)}, 500   

    finally:
        cur.close()
        release_conn(conn)
# =========================
# ACTUALIZAR CONFIG
# =========================
@app.route('/config', methods=['POST'])
def set_config():
    data = request.get_json()

    humedad_objetivo = data.get("humedad_objetivo")
    humedad_objetivo = max(0, min(100, humedad_objetivo))

    tserial = data.get("tserial")
    tserial = max(10, min(100, float(tserial)))

    conn = get_conn()
    cur = conn.cursor()

    try:
        # 🔥 BLOQUEO EN EJECUCIÓN
        cur.execute("SELECT estado_control FROM config_actual WHERE id = 1")
        estado = cur.fetchone()[0]

        if estado == 1:
            return jsonify({"error": "Sistema en ejecución"}), 400

        cur.execute("""
            UPDATE config_actual
            SET modoManual=%s,
                pwm=%s,
                kp=%s,
                ki=%s,
                velA=%s,
                velB=%s,
                humedad_objetivo=%s,
                tserial=%s,
                updated_at=CURRENT_TIMESTAMP
            WHERE id=1
        """, (
            data.get("modoManual"),
            data.get("pwm"),
            data.get("kp"),
            data.get("ki"),
            data.get("velA"),
            data.get("velB"),
            humedad_objetivo,
            tserial
        ))

        conn.commit()

        return jsonify({"status": "ok"})

    except Exception as e:
        conn.rollback()
        print("ERROR CONFIG POST:", e)
        return {"error": str(e)}, 500

    finally:
        cur.close()
        release_conn(conn)

# =========================
# START EXPERIMENTO
# =========================
@app.route('/start', methods=['POST'])
def start():
    data = request.get_json()

    nombre = data.get("nombre", "exp").strip()

    if not nombre:
        nombre = "exp"

    conn = get_conn()
    cur = conn.cursor()
    try:
        # 🔥 EVITAR DOBLE START
        cur.execute("SELECT estado_control FROM config_actual WHERE id=1")
        estado = cur.fetchone()[0]

        if estado == 1:
            return jsonify({"error": "ya está ejecutando"}), 400

        # crear experimento
        cur.execute("""
        INSERT INTO experimentos (
            nombre, modoManual, pwm, kp, ki, velA, velB, humedad_objetivo, tserial
        )
        SELECT
            %s, modoManual, pwm, kp, ki, velA, velB, humedad_objetivo, tserial
        FROM config_actual
        WHERE id=1
        RETURNING id
        """, (nombre,))

        exp_id = cur.fetchone()[0]

        # activar sistema
        cur.execute("""
            UPDATE config_actual
            SET estado_control = 1
            WHERE id = 1
        """)

        conn.commit()

    except Exception as e:
        conn.rollback()
        print("ERROR START:", e)
        return {"error": str(e)}, 500
    
    finally:
        cur.close()
        release_conn(conn)    

    return jsonify({"id": exp_id})

# =========================
# STOP EXPERIMENTO
# =========================
@app.route('/stop', methods=['POST'])
def stop():
    conn = get_conn()
    cur = conn.cursor()

    try:
        # cerrar experimento
        cur.execute("""
            UPDATE experimentos
            SET fecha_fin = CURRENT_TIMESTAMP
            WHERE fecha_fin IS NULL
        """)

        # volver a reposo
        cur.execute("""
            UPDATE config_actual
            SET estado_control = 0,
            pwm = 0,
            velA = 0,
            velB = 0,
            kp = 0,
            ki = 0,
            humedad_objetivo = 0,
            tserial = 15
            WHERE id = 1
        """)

        conn.commit()

    except Exception as e:
        conn.rollback()
        print("ERROR STOP:", e)
        return {"error": str(e)}, 500

    finally:
        cur.close()
        release_conn(conn)

    return jsonify({"status": "ok"})

# =========================
# DESCARGAR CSV
# =========================
@app.route('/download')
def download():

    conn = get_conn()
    cur = conn.cursor()

    try:
        # 🔴 BLOQUEO
        cur.execute("SELECT estado_control FROM config_actual WHERE id=1")
        estado = cur.fetchone()[0]

        if estado == 1:
            return {"error": "Detén el experimento antes de descargar"}, 400

        # 🔥 último experimento
        cur.execute("""
            SELECT id, nombre, fecha_inicio, fecha_fin,
                modoManual, pwm, kp, ki, velA, velB, humedad_objetivo, tserial
            FROM experimentos
            ORDER BY id DESC LIMIT 1
        """)
        exp = cur.fetchone()

        if not exp:
            return {"error": "no hay experimento"}, 400

        exp_id, nombre, inicio, fin, modoManual, pwm, kp, ki, velA, velB, humedad_objetivo, tserial = exp

        # 🔥 marcar como descargado
        cur.execute("""
            UPDATE experimentos
            SET descargado = TRUE
            WHERE id = %s
        """, (exp_id,))
        conn.commit()

        # 🔥 traer datos
        cur.execute("""
            SELECT timestamp, tExt, hExt, tInt, hInt,
                c1, c2, c12, puntoRocio, error,
                pwm, velA, velB, corriente, estado
            FROM datos
            WHERE experiment_id = %s
            ORDER BY timestamp ASC
        """, (exp_id,))

        rows = cur.fetchall()

        # 🔒 cerrar antes de generar
        cur.close()
        release_conn(conn)

        # 🔥 GENERADOR (AHORA SÍ BIEN UBICADO)
        def generate():

            yield f"Experimento:,{nombre}\n"
            yield f"Inicio:,{inicio}\n"
            yield f"Fin:,{fin}\n\n"

            modo = "Manual" if modoManual == 1 else "Auto"

            yield f"Modo:,{modo}\n"
            yield f"PWM:,{pwm}\n"
            yield f"KP:,{kp}\n"
            yield f"KI:,{ki}\n"
            yield f"VelA:,{velA}\n"
            yield f"VelB:,{velB}\n"
            yield f"Humedad objetivo:,{humedad_objetivo}\n"
            yield f"Intervalo serial:,{tserial}\n\n"

            header = [
                "timestamp","tExt","hExt","tInt","hInt",
                "c1","c2","c12","puntoRocio","error",
                "pwm","velA","velB","corriente","estado"
            ]
            yield ",".join(header) + "\n"

            for r in rows:
                yield ",".join([str(x) for x in r]) + "\n"

        return Response(
            generate(),
            mimetype='text/csv',
            headers={
                "Content-Disposition":
                f"attachment; filename=exp_{nombre}_{exp_id}.csv"
            }
        )

    except Exception as e:
        conn.rollback()
        print("ERROR DATA:", e)
        return {"error": str(e)}, 500

    finally:
        pass

# =========================
# EXPERIMENTO ACTIVO
# =========================

# =========================
# BORRAR DATOS
# =========================
@app.route('/clear', methods=['POST'])
def clear():

    conn = get_conn()
    cur = conn.cursor()

    try:
        # 🔥 último experimento
        cur.execute("""
            SELECT id, descargado
            FROM experimentos
            ORDER BY id DESC LIMIT 1
        """)
        exp = cur.fetchone()

        if not exp or not exp[1]:
            return {"error": "Debes descargar primero"}, 400

        # 🔥 borrar datos
        cur.execute("DELETE FROM datos WHERE experiment_id = %s", (exp[0],))

        conn.commit()

    except Exception as e:
        conn.rollback()
        print("ERROR DATA:", e)
        return {"error": str(e)}, 500

    finally:
        cur.close()
        release_conn(conn)

    return {"status": "ok"}

# =========================
# RECIBIR DATOS
# =========================
@app.route('/data', methods=['POST'])
def data():
    global latest_data

    data = request.get_json()
    if not data:
        return jsonify({"error": "sin datos"}), 400

    # 🔥 siempre actualizar estado online
    latest_data = {
        "data": data,
        "last_seen": time.time()        
    }

    print("📥 Datos recibidos:", data)

    conn = get_conn()
    cur = conn.cursor()

    try:
        # estado sistema
        cur.execute("SELECT estado_control FROM config_actual WHERE id=1")
        estado = cur.fetchone()[0]

        # 🔴 NO GUARDAR EN REPOSO
        if estado == 0:
            return jsonify({"status": "reposo"})

        cur.execute("""
            SELECT id FROM experimentos
            WHERE fecha_fin IS NULL
            ORDER BY id DESC LIMIT 1
        """)

        r = cur.fetchone()
        exp_id = r[0] if r else None

        if exp_id is None:
            return jsonify({"error": "no hay experimento"}), 400

        cur.execute("""
            INSERT INTO datos (
                experiment_id,
                tExt,hExt,tInt,hInt,
                c1,c2,c12,
                puntoRocio,error,
                pwm,velA,velB,
                corriente,estado
            )
            VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s)
        """, (
            exp_id,
            data.get("tExt"),
            data.get("hExt"),
            data.get("tInt"),
            data.get("hInt"),
            data.get("c1"),
            data.get("c2"),
            data.get("c12"),
            data.get("puntoRocio"),
            data.get("error"),
            data.get("pwm"),
            data.get("velA"),
            data.get("velB"),
            data.get("corriente"),
            data.get("estado")
        ))

        conn.commit()

        return jsonify({"status": "guardado"})

    except Exception as e:
        conn.rollback()
        print("ERROR DATA:", e)
        return {"error": str(e)}, 500

    finally:
        cur.close()
        release_conn(conn)
# =========================
# STATUS
# =========================
@app.route('/status')
def status():
    global latest_data

    ahora = time.time()
    last_seen = latest_data.get("last_seen", 0)

    online = (ahora - last_seen) < 10

    if not online:
        return {"online": False}

    conn = get_conn()
    cur = conn.cursor()

    cur.execute("SELECT estado_control FROM config_actual WHERE id=1")
    estado = cur.fetchone()[0]

    cur.close()
    release_conn(conn)

    return {
        "online": True,
        "estado_control": estado
    }

# =========================
# RUN
# =========================
if __name__ == "__main__":
    app.run(
    host="0.0.0.0",
    port=5000,
    debug=False,
    threaded=True
)