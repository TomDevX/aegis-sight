import serial
import threading
import os
import sys

# Thay 'COM3' bằng cổng COM thực tế của ESP32-S3
COM_PORT = 'COM8' 
BAUD_RATE = 115200
OUTPUT_FILE = 'gemini_tts.mp3'

try:
    ser = serial.Serial(COM_PORT, BAUD_RATE, timeout=1)
    print(f"=============================================")
    print(f" Connected to ESP32 on {COM_PORT}")
    print(f" Gõ câu hỏi trực tiếp vào đây rồi nhấn Enter.")
    print(f"=============================================\n")
except Exception as e:
    print(f"[LỖI] Không thể mở cổng {COM_PORT}: {e}")
    sys.exit(1)

# Luồng 1: Liên tục đọc dữ liệu từ ESP32 và tải file MP3
def read_from_esp32():
    receiving = False
    mp3_data = bytearray()

    while True:
        try:
            line = ser.readline()
            if not line:
                continue
                
            # Kiểm tra cờ bắt đầu nhận MP3
            if b'<<<START_MP3_TRANSFER>>>' in line:
                print("\n[PC] Đang nhận dữ liệu MP3 từ ESP32...")
                receiving = True
                mp3_data = bytearray()
                continue

            # Kiểm tra cờ kết thúc MP3
            if b'<<<END_MP3_TRANSFER>>>' in line:
                print(f"[PC] Tải xong! Đang lưu thành {OUTPUT_FILE}...")
                with open(OUTPUT_FILE, 'wb') as f:
                    f.write(mp3_data)
                    
                print(f"[SUCCESS] Đã tạo file: {os.path.abspath(OUTPUT_FILE)}")
                
                # Tự động phát file MP3 trên Windows
                os.system(f'start {OUTPUT_FILE}')
                receiving = False
                print("\n> ", end='', flush=True)
                continue

            if receiving:
                mp3_data.extend(line)
            else:
                # Hiển thị log text thường của ESP32
                try:
                    text = line.decode('utf-8', errors='ignore')
                    if text.strip():
                        print(text, end='')
                except:
                    pass

        except Exception as e:
            break

# Chạy luồng đọc ngầm
thread = threading.Thread(target=read_from_esp32, daemon=True)
thread.start()

# Luồng chính: Cho phép bạn NHẬP CÂU HỎI trực tiếp từ Terminal Python
try:
    while True:
        question = input("> ")
        if question.strip():
            if question.strip().lower() == 'quit':
                break
            # Gửi câu hỏi qua cổng Serial cho ESP32
            ser.write((question.strip() + "\n").encode('utf-8'))
except KeyboardInterrupt:
    print("\nĐã dừng chương trình.")

ser.close()