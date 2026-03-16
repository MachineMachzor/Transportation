import cv2
import requests
import numpy as np

import time

from flask import Flask, send_file, jsonify

from playwright.sync_api import sync_playwright



app = Flask(__name__)


# "ipconfig" in cmd
# IPv4
# https://github.com/IDEA-Research/GroundingDINO
baseHost = "192.168.0.84" #Internet
baseHost = "10.45.148.80" #Phone


save_api_key_loc = r"C:\Users\ringk\OneDrive\Documents\PlatformIO\Projects\Transportation_IO\apiKey.txt"


# http://192.168.0.84/refresh
@app.route('/refresh', methods=['GET'])
def api_key_refresh():
    test_url = "https://wego.here.com/r/publicTransport/s-Yz07aWQ9aGVyZSUzQWFmJTNBc3RyZWV0c2VjdGlvbiUzQTFXc01kOVVpS05GS3I5QlMtR0c0bkQlM0FDZ2NJQkNDU3Blb2pFQUVhQkRNeE1UYztsYXQ9NDAuODEzMzY7bG9uPS03My45NjAzMztuPTMxMTclMjBCcm9hZHdheSUyQyUyME5ldyUyMFlvcmslMkMlMjBOWSUyMDEwMDI3LTQ2MDklMkMlMjBVbml0ZWQlMjBTdGF0ZXM7cGg9/s-Yz07aWQ9aGVyZSUzQWFmJTNBc3RyZWV0c2VjdGlvbiUzQVVNbHlLT1k3cUwyYXlFcUNQaGtXNEElM0FDZ2NJQkNET3lla2pFQUVhQkRJeU9EYztsYXQ9NDAuNzk3MTM7bG9uPS03My45MzQ4MTtuPTIyODclMjAxc3QlMjBBdmUlMkMlMjBOZXclMjBZb3JrJTJDJTIwTlklMjAxMDAzNS01MDU3JTJDJTIwVW5pdGVkJTIwU3RhdGVzO3BoPQ==?map=40.80492,-73.94577,14.45"
    # resp = requests.get(test_url, verify=False)
    with sync_playwright() as p:
        browser = p.chromium.launch(headless=True)  # IMPORTANT
        page = browser.new_page()

        def handle_request(req):
            url = req.url
            if "router.hereapi.com" in url and "routes" in url:
                apiKey = url.split("apiKey=")[1].split("&destination=")[0]
                print(apiKey)

                with open(save_api_key_loc, 'w') as f:
                    f.write(apiKey)


                print("ROUTING REQUEST:", url)
                # print("REQUEST HEADERS:", req.headers)

        page.on("request", handle_request)

        page.goto(test_url, wait_until="networkidle") #JS Bundle needs to load for app

        # Give the JS app time to initialize routing
        time.sleep(5)



        browser.close()
    return "OK"
    

@app.route('/get_api_key', methods=['GET'])
def api_key_get():
    with open(save_api_key_loc, 'r') as f:
        api_key = f.readline()
        return api_key
    return ''




if __name__ == '__main__':
    app.run(host=baseHost, port=5000) #threaded=True, use_reloader=False
