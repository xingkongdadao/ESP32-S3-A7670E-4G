#!/usr/bin/env python3
"""
at_apn_tool.py
自动探测串口、打印 AT 查询结果；可选设置 APN 并激活 PDP。
依赖: pyserial
"""
import sys
import time
import glob
import argparse
import serial

DEFAULT_BAUD = 115200
READ_TIMEOUT = 2.0

COMMON_PORT_PATTERNS = ["*usbmodem*", "*usbserial*", "*SLAB_USBtoUART*", "*cu.SLAB_USBtoUART*", "*cu.usbserial*"]

AT_SEQUENCE = [
    ("ATE0","关闭回显（可选）"),
    ("AT","基本连通性"),
    ("AT+CPIN?","SIM 状态"),
    ("AT+CSQ","信号强度"),
    ("AT+COPS?","当前运营商"),
    ("AT+CREG?","网络注册（2G/3G）"),
    ("AT+CEREG?","网络注册（EPS/LTE）"),
    ("AT+CGREG?","GPRS 注册"),
    ("AT+CGDCONT?","PDP context（APN）"),
    ("AT+CGATT?","是否附着到分组域"),
    ("AT+CFUN?","模块功能模式")
]

def list_ports():
    ports = []
    for pattern in COMMON_PORT_PATTERNS:
        ports += glob.glob("/dev/" + pattern)
    # fallback: list all cu.*
    if not ports:
        ports = glob.glob("/dev/cu.*")
    # dedupe and sort
    return sorted(set(ports))

def choose_port_auto():
    ports = list_ports()
    if not ports:
        return None
    # prefer ones containing usbmodem / usbserial
    for p in ports:
        if "usbmodem" in p or "usbserial" in p or "SLAB" in p or "cu.SLAB" in p:
            return p
    return ports[0]

def open_serial(port, baud):
    ser = serial.Serial(port, baud, timeout=READ_TIMEOUT)
    # small delay for some modules
    time.sleep(0.2)
    return ser

def send_cmd(ser, cmd, wait=0.2, read_more=True):
    cmd_str = cmd.strip() + "\r\n"
    ser.reset_input_buffer()
    ser.write(cmd_str.encode())
    time.sleep(wait)
    out = []
    deadline = time.time() + READ_TIMEOUT
    while time.time() < deadline:
        try:
            line = ser.readline()
        except Exception:
            break
        if not line:
            break
        try:
            decoded = line.decode(errors="ignore").rstrip()
        except Exception:
            decoded = repr(line)
        out.append(decoded)
        # if we see OK or ERROR we can stop earlier
        if decoded in ("OK","ERROR"):
            break
    return out

def print_block(title, lines):
    print("\n---- {} ----".format(title))
    for l in lines:
        print(l)
    print("---- end {} ----\n".format(title))

def probe_all(ser):
    results = {}
    for cmd,desc in AT_SEQUENCE:
        lines = send_cmd(ser, cmd)
        results[cmd] = lines
        print_block(f"{cmd}  ({desc})", lines if lines else ["<no response>"])
    return results

def set_apn_and_activate(ser, apn):
    print(f"\n设置 APN: {apn}")
    # set PDP context
    lines = send_cmd(ser, f'AT+CGDCONT=1,"IP","{apn}"', wait=0.3)
    print_block('AT+CGDCONT', lines if lines else ['<no response>'])
    # ensure attached
    lines = send_cmd(ser, "AT+CGATT?")
    print_block("AT+CGATT? (before)", lines)
    if any("0" in l for l in lines if "+CGATT" in l) or (not any("1" in l for l in lines if "+CGATT" in l)):
        print("尝试附着：AT+CGATT=1")
        lines = send_cmd(ser, "AT+CGATT=1", wait=1.0)
        print_block("AT+CGATT=1", lines)
        time.sleep(1)
    # activate PDP
    print("尝试激活 PDP: AT+CGACT=1,1")
    lines = send_cmd(ser, "AT+CGACT=1,1", wait=1.0)
    print_block("AT+CGACT=1,1", lines)
    time.sleep(1)
    # 查询 IP
    lines = send_cmd(ser, "AT+CGPADDR", wait=0.5)
    print_block("AT+CGPADDR (after activation)", lines)
    return lines

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", "-p", help="串口设备路径，例如 /dev/cu.usbmodem123")
    parser.add_argument("--baud", "-b", type=int, default=DEFAULT_BAUD)
    parser.add_argument("--apn", help="如果提供 APN，脚本会尝试设置并激活 PDP")
    args = parser.parse_args()

    port = args.port
    if not port:
        port = choose_port_auto()
        if port:
            print(f"自动选择串口: {port}")
        else:
            print("未能自动检测到串口，请手动指定 --port")
            sys.exit(1)
    try:
        ser = open_serial(port, args.baud)
    except Exception as e:
        print(f"打开串口失败: {e}")
        sys.exit(1)

    try:
        print("\n开始探测基础 AT 状态，下面的全部输出请复制并粘贴回来：")
        probe_all(ser)

        if args.apn:
            set_apn_and_activate(ser, args.apn)
        else:
            print("\n如果需要我帮你设置 APN 并激活 PDP，请重新运行并加上 --apn your_apn")
            print("示例: python3 at_apn_tool.py --port /dev/cu.usbmodem123 --apn internet")
    finally:
        try:
            ser.close()
        except:
            pass

if __name__ == "__main__":
    main()