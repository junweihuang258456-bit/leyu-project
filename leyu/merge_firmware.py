import json
import os
import subprocess
import sys

def main():
    build_dir = 'build'
    json_path = os.path.join(build_dir, 'flasher_args.json')
    output_bin = 'merged_firmware.bin'

    if not os.path.exists(json_path):
        print(f"错误: 找不到 {json_path}。请先编译工程 (idf.py build)。")
        sys.exit(1)

    with open(json_path, 'r', encoding='utf-8') as f:
        args = json.load(f)

    # 读取烧录配置
    flash_mode = args.get('flash_settings', {}).get('flash_mode', 'dio')
    flash_freq = args.get('flash_settings', {}).get('flash_freq', '80m')
    flash_size = args.get('flash_settings', {}).get('flash_size', '16MB')
    chip = args.get('extra_esptool_args', {}).get('chip', 'esp32s3')

    cmd = [
        sys.executable, '-m', 'esptool', '--chip', chip, 'merge_bin',
        '-o', output_bin,
        '--flash_mode', flash_mode,
        '--flash_freq', flash_freq,
        '--flash_size', flash_size
    ]

    # 获取所有需要合并的 bin 文件及其偏移地址
    files = args.get('flash_files', {})
    for offset, file_path in files.items():
        full_path = os.path.join(build_dir, file_path)
        if not os.path.exists(full_path):
            print(f"警告: 找不到文件 {full_path}，合并出的固件可能不完整！")
        cmd.extend([offset, full_path])

    print("开始合并固件...")
    print(f"执行命令: {' '.join(cmd)}")

    try:
        subprocess.run(cmd, check=True)
        print(f"\n[成功] 固件合并完成！输出文件: {os.path.abspath(output_bin)}")
    except subprocess.CalledProcessError as e:
        print(f"\n[失败] 合并过程发生错误！")
        sys.exit(1)
    except FileNotFoundError:
        print("\n[错误] 找不到 esptool.py。请确保你是在 ESP-IDF 环境终端中运行此脚本！")
        sys.exit(1)

if __name__ == "__main__":
    main()
