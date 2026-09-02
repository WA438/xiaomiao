#!/usr/bin/env python3
"""
MQTT 命令中继 — 沙盒端
通过公共 MQTT Broker 桥接 TRAE ↔ ZeroTermux
不再依赖变化的 IP，使用固定 Broker 地址
"""
import json, time, threading, uuid, os
import paho.mqtt.client as mqtt

BROKER = "broker.emqx.io"
PORT = 1883
TOPIC_CMD = "xiaomiao/cmd"
TOPIC_RESULT = "xiaomiao/result"
SESSION_ID = str(uuid.uuid4())[:8]

CMD_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), '.cmd_pending.json')
RESULT_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), '.cmd_result.json')
LOCK = threading.Lock()

def read_json(path):
    with LOCK:
        try:
            if os.path.exists(path):
                with open(path) as f:
                    return json.load(f)
        except: pass
    return []

def write_json(path, data):
    with LOCK:
        with open(path, 'w') as f:
            json.dump(data, f, ensure_ascii=False, indent=2)

def on_connect(client, userdata, flags, rc):
    print(f"[MQTT] 已连接 broker.emqx.io (rc={rc})")
    client.subscribe(TOPIC_RESULT)

def on_message(client, userdata, msg):
    try:
        payload = json.loads(msg.payload.decode())
        output = payload.get('output', '')
        if payload.get('exit_code', 0) != 0:
            output += f'\n(exit={payload["exit_code"]})'
        result = {
            'id': str(int(time.time() * 1000)),
            'output': output,
            'time': time.strftime('%H:%M:%S'),
            'timestamp': time.time()
        }
        results = read_json(RESULT_FILE)
        results.append(result)
        if len(results) > 50:
            results = results[-50:]
        write_json(RESULT_FILE, results)
        print(f"[MQTT] 收到结果: {result['time']} ({len(output)}字节)")
    except Exception as e:
        print(f"[MQTT] 解析结果失败: {e}")

def main():
    print(f"=== MQTT 中继 (沙盒端) ===")
    print(f"Broker: {BROKER}:{PORT}")
    print(f"Session: {SESSION_ID}")
    
    client = mqtt.Client(client_id=f"trae-relay-{SESSION_ID}")
    client.on_connect = on_connect
    client.on_message = on_message
    
    client.connect(BROKER, PORT, 60)
    client.loop_start()
    
    print(f"[MQTT] 已启动，等待命令...")
    print(f"[MQTT] 发命令: 写入 {CMD_FILE}")
    
    # 主循环：检查本地命令文件，发布到 MQTT
    while True:
        try:
            cmds = read_json(CMD_FILE)
            if cmds:
                cmd = cmds.pop(0)
                write_json(CMD_FILE, cmds)
                client.publish(TOPIC_CMD, json.dumps(cmd, ensure_ascii=False))
                print(f"[MQTT] 已发布命令: {cmd.get('cmd', '')[:50]}...")
        except Exception as e:
            print(f"[MQTT] 错误: {e}")
        time.sleep(1)

if __name__ == '__main__':
    main()