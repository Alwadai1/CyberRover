"""
CyberRover - Control & Monitoring Console
لوحة المراقبة: تعرض نتائج الجولة من data/scans.jsonl

Run:  streamlit run dashboard/app.py
"""
import json
from pathlib import Path

import pandas as pd
import streamlit as st

st.set_page_config(page_title="CyberRover Console", page_icon="🤖", layout="wide")
st.title("CyberRover - Control & Monitoring Console")
st.caption("لوحة المراقبة والتحكم · مسح سلبي داخل نطاق المنشأة المصرّح بها")

path = Path(st.sidebar.text_input("ملف البيانات", "ai/data/scans.jsonl"))
if not path.exists():
    st.info("لا توجد بيانات بعد. شغّل ai/serial_logger.py أثناء الجولة، ثم حدّث الصفحة.")
    st.stop()

records = [json.loads(l) for l in path.read_text(encoding="utf-8").splitlines() if l.strip()]
df = pd.DataFrame([{
    "الوقت": r.get("time", ""), "نقطة الفحص": r["checkpoint"], "أفضل إشارة dBm": r["best_rssi"],
    "التغطية": r["coverage"], "عدد الشبكات": r["network_count"], "التنبيهات": len(r.get("alerts", [])),
} for r in records])

c1, c2, c3, c4 = st.columns(4)
c1.metric("نقاط مفحوصة", df["نقطة الفحص"].nunique())
c2.metric("متوسط الإشارة", f"{df['أفضل إشارة dBm'].mean():.0f} dBm")
c3.metric("نقاط عمياء", int((df["التغطية"] == "DEAD_ZONE").sum()))
c4.metric("تهديدات", int(sum(1 for r in records for a in r.get("alerts", []) if a["type"] != "DEAD_ZONE")))

st.subheader("خريطة التغطية حسب نقاط الفحص")
latest = df.groupby("نقطة الفحص").last().reset_index()
st.bar_chart(latest.set_index("نقطة الفحص")["أفضل إشارة dBm"])

st.subheader("التنبيهات")
alerts = [{"نقطة الفحص": r["checkpoint"], "النوع": a["type"], "SSID": a.get("ssid", ""), "BSSID": a.get("bssid", "")}
          for r in records for a in r.get("alerts", [])]
st.dataframe(pd.DataFrame(alerts) if alerts else pd.DataFrame([{"الحالة": "لا توجد تنبيهات"}]), use_container_width=True)

st.subheader("سجل الجولة")
st.dataframe(df, use_container_width=True)

cp = st.selectbox("تفاصيل الشبكات في نقطة", latest["نقطة الفحص"])
rec = [r for r in records if r["checkpoint"] == cp][-1]
st.dataframe(pd.DataFrame(rec["networks"]), use_container_width=True)
