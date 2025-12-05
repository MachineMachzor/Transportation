

from selenium import webdriver
from selenium.webdriver.common.by import By
from selenium.webdriver.chrome.options import Options
from webdriver_manager.chrome import ChromeDriverManager
from PIL import Image, ImageOps, ImageFilter
import pytesseract
import time
import io
from selenium.webdriver.chrome.service import Service

from threading import Thread, Lock


from flask import Flask, send_file, jsonify



app = Flask(__name__)


baseHost = "http://192.168.0.88"
url = f"{baseHost}/time"


# https://time.is/


@app.route('/datetime', methods=['GET'])
def get_datetime():
    pass


# https://time.is/t1/?en.0.10.955.0P.-300.161.1764973717963.1764973716389..N



chrome_opts = Options()
chrome_opts.add_argument("--headless=new")   # or "--headless" depending on Chrome version
chrome_opts.add_argument("--disable-gpu")
chrome_opts.add_argument("--no-sandbox")
chrome_opts.add_argument("--window-size=1200,800")

chromeDriverPath = r"C:\Users\ringk\OneDrive\chromedriver.exe"
service = Service(chromeDriverPath)
driver = webdriver.Chrome(service=service, options=chrome_opts)

driver.get("https://time.is/")



timeout = 15
end = time.time() + timeout
elem = None
while time.time() < end:
    try:
        elem = driver.find_element(By.CSS_SELECTOR, "#clock0_bg")
        if elem.is_displayed():
            break
    except Exception:
        pass
    time.sleep(0.2)
if elem is None:
    raise RuntimeError("Element #clock0_bg not found")

