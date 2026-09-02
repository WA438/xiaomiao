#!/usr/bin/env python3
"""
TRAE ↔ ZeroTermux paste.rs 命令中继
不依赖 IP，通过 paste.rs 交换命令和结果

用法:
  python3 paste_relay.py send <命令>    — 发送命令到 ZeroTermux
  python3 paste_relay.py result          — 读取最新结果
  python3 paste_relay.py watch           — 持续监控结果
  python3 paste_relay.py init            — 初始化 genesis
"""

import os, sys, json, time, base64, subprocess

RELAY_DIR = os.path.dirname(os.path.abspath(__file__))
GENESIS_FILE = os.path.join(RELAY_DIR, '.paste_genesis.json')
RESULT_FILE = os.path.join(RELAY_DIR, '.paste_results.json')

def curl(url, data=None, method='GET'):
    """发送 HTTP 请求"""
    cmd = ['curl', '-s', '--max-time', '15']
    if data:
        cmd += ['--data-binary', '@-']
    cmd.append(url)
    try:
        input_data = data if isinstance(data, bytes) else data.encode() if data else None
        p = subprocess.run(cmd, input=input_data, capture_output=True, timeout=20)
        return p.stdout.decode('utf-8', errors='replace').strip()
    except Exception as e:
        print(f'  [curl error] {e}')
        return ''

def create_paste(content):
    """创建 paste 并返回 URL"""
    return curl('https://paste.rs/', data=content)

def read_paste(url):
    """读取 paste 内容"""
    return curl(url)

def get_genesis():
    """读取 genesis 文件"""
    try:
        with open(GENESIS_FILE, 'r') as f:
            return json.load(f)
    except:
        return {'genesis_url': '', 'cmd_url': '', 'result_url': ''}

def save_genesis(data):
    with open(GENESIS_FILE, 'w') as f:
        json.dump(data, f, ensure_ascii=False, indent=2)

def get_results():
    try:
        with open(RESULT_FILE, 'r') as f:
            return json.load(f)
    except:
        return []

def save_results(results):
    with open(RESULT_FILE, 'w') as f:
        json.dump(results, f, ensure_ascii=False, indent=2)

def cmd_init():
    """初始化 genesis paste"""
    genesis = get_genesis()
    data = json.dumps({'type': 'genesis', 'next_cmd': '', 'next_result': '', 'ts': time.time()})
    genesis_url = create_paste(data)
    if not genesis_url:
        print("ERROR: 无法创建 genesis paste")
        sys.exit(1)
    genesis['genesis_url'] = genesis_url
    save_genesis(genesis)
    print(f"GENESIS={genesis_url}")
    print(f"已保存到 {GENESIS_FILE}")

def cmd_send(command):
    """发送命令"""
    genesis = get_genesis()
    if not genesis.get('genesis_url'):
        print("请先运行 init 初始化")
        sys.exit(1)

    # 创建命令 paste
    cmd_data = json.dumps({'type': 'cmd', 'cmd': command, 'ts': time.time()})
    cmd_url = create_paste(cmd_data)
    if not cmd_url:
        print("ERROR: 无法创建命令 paste")
        sys.exit(1)

    # 更新 genesis
    genesis['cmd_url'] = cmd_url
    genesis['result_url'] = ''
    gen_data = json.dumps({
        'type': 'genesis',
        'next_cmd': cmd_url,
        'next_result': '',
        'ts': time.time()
    })
    new_genesis = create_paste(gen_data)
    if new_genesis:
        genesis['genesis_url'] = new_genesis
    save_genesis(genesis)

    print(f"命令已发送: {command}")
    print(f"CMD_URL={cmd_url}")
    print(f"GENESIS={genesis['genesis_url']}")

def cmd_result():
    """读取最新结果"""
    results = get_results()
    if results:
        latest = results[-1]
        print(f"[{latest.get('time', '?')}] 结果:")
        print(latest.get('output', '(空)'))
        print(f"--- 共 {len(results)} 条结果 ---")
    else:
        print("暂无结果")
        # 尝试从 genesis 读取
        genesis = get_genesis()
        if genesis.get('result_url'):
            content = read_paste(genesis['result_url'])
            if content:
                try:
                    data = json.loads(content)
                    if data.get('type') == 'result':
                        result = {
                            'id': str(int(time.time() * 1000)),
                            'output': data.get('output', ''),
                            'time': data.get('time', time.strftime('%H:%M:%S')),
                            'timestamp': time.time()
                        }
                        results.append(result)
                        save_results(results)
                        print(f"[{result['time']}] 结果:")
                        print(result['output'])
                except:
                    print(f"原始内容: {content[:200]}")

def cmd_watch():
    """持续监控结果"""
    print("监控结果中... (Ctrl+C 停止)")
    last_count = len(get_results())
    try:
        while True:
            time.sleep(2)
            # 检查 genesis 中的 result_url
            genesis = get_genesis()
            if genesis.get('result_url'):
                content = read_paste(genesis['result_url'])
                if content:
                    try:
                        data = json.loads(content)
                        if data.get('type') == 'result':
                            results = get_results()
                            result = {
                                'id': str(int(time.time() * 1000)),
                                'output': data.get('output', ''),
                                'time': data.get('time', time.strftime('%H:%M:%S')),
                                'timestamp': time.time()
                            }
                            # 检查是否已存在
                            if not results or results[-1].get('output') != result['output']:
                                results.append(result)
                                save_results(results)
                                print(f"\n[{result['time']}] 收到结果:")
                                print(result['output'])
                                print("---")
                    except:
                        pass

            # 也检查本地结果文件
            results = get_results()
            if len(results) > last_count:
                for r in results[last_count:]:
                    print(f"\n[{r.get('time', '?')}] 结果:")
                    print(r.get('output', ''))
                last_count = len(results)
    except KeyboardInterrupt:
        print("\n停止监控")

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("用法: python3 paste_relay.py <init|send|result|watch>")
        print("  init              — 初始化 genesis paste")
        print("  send <命令>       — 发送命令")
        print("  result            — 查看最新结果")
        print("  watch             — 持续监控结果")
        sys.exit(1)

    action = sys.argv[1]
    if action == 'init':
        cmd_init()
    elif action == 'send':
        if len(sys.argv) < 3:
            print("用法: python3 paste_relay.py send <命令>")
            sys.exit(1)
        cmd_send(' '.join(sys.argv[2:]))
    elif action == 'result':
        cmd_result()
    elif action == 'watch':
        cmd_watch()
    else:
        print(f"未知操作: {action}")
        sys.exit(1)