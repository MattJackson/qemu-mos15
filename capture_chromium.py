#!/usr/bin/env python3
import subprocess, json, os, time, glob
from datetime import datetime

OUTPUT_DIR = "/home/matthew/mos-docker/logs/vnc-screenshots"
os.makedirs(OUTPUT_DIR, exist_ok=True)

def take_screenshot(port):
    timestamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    env = os.environ.copy()
    env["DISPLAY"] = ":99"
    result = subprocess.run(
        ["chromium-browser", "--headless", "--disable-gpu", "--no-sandbox",
         f"--screenshot={OUTPUT_DIR}/phase-0-{timestamp}.png",
         "--window-size=1920,1080", 
         "http://localhost:6080/vnc.html?host=localhost&port=6080"],
        capture_output=True, timeout=30, env=env
    )
    return result.returncode == 0

def analyze_screenshot(png_path):
    from PIL import Image
    img = Image.open(png_path).convert("RGB")
    pixels = list(img.getdata())
    width, height = img.size
    
    if len(pixels) == 0:
        return {"state": "empty"}
    
    mean_bright = sum(sum(p[:3]) for p in pixels) / len(pixels) / 765.0
    
    mid_y = height // 2
    top_half = pixels[:len(pixels)//2]
    bottom_half = pixels[len(pixels)//2:]
    
    top_bright = sum(sum(p[:3]) for p in top_half) / max(len(top_half), 1) / 765.0 if top_half else mean_bright
    bottom_bright = sum(sum(p[:3]) for p in bottom_half) / max(len(bottom_half), 1) / 765.0
    
    img_gray = img.convert("L")
    gray_pixels = list(img_gray.getdata())
    
    edge_count = 0
    for y in range(1, height-1):
        for x in range(1, width-1):
            center = gray_pixels[y * width + x]
            neighbors = (gray_pixels[(y-1)*width+x] + gray_pixels[(y+1)*width+x] + 
                        gray_pixels[y*width+(x-1)] + gray_pixels[y*width+(x+1)]) / 4
            if abs(center - neighbors) > 25:
                edge_count += 1
    
    edge_density = edge_count / (width * height)
    
    if mean_bright < 0.03:
        state = "black_screen"
    elif bottom_bright > top_bright + 0.2 and edge_density > 0.05:
        state = "login_screen_or_ui_bottom"
    elif edge_density > 0.12 and mean_bright > 0.3 and mean_bright < 0.7:
        state = "text_or_uefi_shell"
    elif mean_bright > 0.85:
        state = "bright_white_screen"
    else:
        state = f"uniform_content_{mean_bright:.2f}"
    
    return {"state": state, "brightness": round(mean_bright, 4), "edge_density": round(edge_density, 4)}

def main():
    print("Starting Chromium screenshot capture...")
    
    while True:
        timestamp = datetime.now().strftime("%Y%m%d-%H%M%S")
        
        if take_screenshot(6080):
            png_path = f"{OUTPUT_DIR}/phase-0-{timestamp}.png"
            
            try:
                analysis = analyze_screenshot(png_path)
                
                metadata = {
                    "timestamp": datetime.now().isoformat(),
                    **analysis
                }
                
                with open(f"{OUTPUT_DIR}/phase-0-{timestamp}.json", "w") as f:
                    json.dump(metadata, f, indent=2)
                
                print(f"{timestamp} State: {analysis['state']} (bright: {analysis['brightness']:.3f})")
            except Exception as e:
                print(f"Analysis error: {e}")
        else:
            print(f"[{timestamp}] Screenshot capture failed")
        
        time.sleep(5)

if __name__ == "__main__": main()
