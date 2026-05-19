import os
import psycopg2
from psycopg2 import pool
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
                tserial=%s                
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
# RUN
# =========================
if __name__ == "__main__":
    app.run(
    host="0.0.0.0",
    port=5000,
    debug=False,
    threaded=True
)