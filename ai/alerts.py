"""
CyberRover — instant alerts (Telegram)
التنبيهات الفورية عبر Telegram.

Set two environment variables before running:
    TELEGRAM_BOT_TOKEN   token from @BotFather
    TELEGRAM_CHAT_ID     the technician's chat id
If they are not set, alerts are printed to the console only.
"""
import os

import requests

MESSAGES = {
    "EVIL_TWIN": "🚨 توأم شرير: شبكة تحمل اسم شبكة المنشأة من جهاز غير معتمد",
    "CHANNEL_MISMATCH": "⚠️ نقطة وصول معتمدة تبث على قناة غير متوقعة",
    "SECURITY_DOWNGRADE": "⚠️ نقطة وصول معتمدة بتشفير أضعف من السياسة",
    "WEAK_ENCRYPTION": "⚠️ شبكة مفتوحة أو بتشفير WEP",
    "DEAD_ZONE": "📶 نقطة عمياء: الإشارة أضعف من -72 dBm",
    "ANOMALY": "🤖 الذكاء الاصطناعي رصد نمطاً غير معتاد في هذه النقطة",
}


def send_alert(checkpoint: str, alert: dict) -> None:
    kind = alert.get("type", "ALERT")
    text = f"CyberRover · {checkpoint}\n{MESSAGES.get(kind, kind)}"
    if alert.get("ssid"):
        text += f"\nSSID: {alert['ssid']}"
    if alert.get("bssid"):
        text += f"\nBSSID: {alert['bssid']}"
    if alert.get("rssi") is not None:
        text += f"\nRSSI: {alert['rssi']} dBm"
    if alert.get("score") is not None:
        text += f"\nAnomaly score: {alert['score']:.3f}"

    token, chat = os.getenv("TELEGRAM_BOT_TOKEN"), os.getenv("TELEGRAM_CHAT_ID")
    if not token or not chat:
        print("[ALERT]", text.replace("\n", " | "))
        return
    try:
        requests.post(f"https://api.telegram.org/bot{token}/sendMessage",
                      data={"chat_id": chat, "text": text}, timeout=10)
    except requests.RequestException as e:
        print("Telegram error:", e)
