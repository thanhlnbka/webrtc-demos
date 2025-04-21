
# WebRTC Demos: GStreamer + libdatachannel to Browser

This project demonstrates a lightweight WebRTC setup that streams media data from a native C++ backend (using **GStreamer** + **libdatachannel**) to a web browser client, via WebRTC peer-to-peer connection.

## 🎯 Project Goals

- 🔄 **Peer-to-peer WebRTC** connection between a C++ app and a browser
- 🎥 **Media streaming** using [GStreamer](https://gstreamer.freedesktop.org/) (H264 RTP stream)
- 📡 **Signaling server** for WebRTC negotiation (SDP + ICE) using [WebSocket](https://developer.mozilla.org/en-US/docs/Web/API/WebSockets_API)
- 🌐 Simple **web interface** for the receiving peer
- 🔧 Modular design with one sender (`peer1_sender`) and one receiver (`peer2_receiver`)

---

## 📸 Demo

<p align="center">
  <img src="images/demo.gif" width="700" alt="WebRTC GStreamer Demo"/>
</p>

---

## 📁 Project Structure

```bash
webrtc-demos/
├── peer1_sender/
│   └── main.cpp             # C++ WebRTC sender using libdatachannel + GStreamer
├── peer2_receiver/
│   └── index.html           # Browser receiver (HTML + JS WebRTC client)
├── signal_server_1_n.py     # WebSocket signaling server (Python)
├── images/
│   └── demo.gif             # Demo animation
└── README.md                # This file
```

---

## 🚀 How It Works

1. **Signaling**  
   Both peers (sender and receiver) connect to `signal_server_1_n.py` over WebSocket to exchange:
   - SDP offers/answers
   - ICE candidates

2. **C++ Sender**  
   - Launches a [GStreamer](https://gstreamer.freedesktop.org/) pipeline (e.g., `videotestsrc`, `rtph264pay`)
   - Sends RTP packets via [libdatachannel](https://github.com/paullouisageneau/libdatachannel) to the browser

3. **Browser Receiver**  
   - Uses [WebRTC](https://webrtc.org/) via `RTCPeerConnection` API
   - Receives RTP video stream and renders it in `<video>` tag

---

## 🛠️ Getting Started

### 1. Install Dependencies

**On the C++ side:**

- [GStreamer](https://gstreamer.freedesktop.org/) (1.20+)
- [libdatachannel](https://github.com/paullouisageneau/libdatachannel)
- [Boost](https://www.boost.org/) & [websocketpp](https://github.com/zaphoyd/websocketpp)
- CMake 3.16+

### 2. Run Signaling Server

```bash
python3 signal_server_1_n.py
```


### 3. Build Sender

```bash
cd peer1_sender
mkdir build && cd build
cmake ..
make
./peer1_sender <path_video>
```



> Default WebSocket port: `ws://localhost:8765`

### 4. Run Browser Receiver

```bash
cd peer2_receiver
python3 -m http.server 8000
# Then open in browser: http://localhost:8000/index.html
```

---

## 🔧 Configuration

- **STUN/TURN servers**: configurable in both `main.cpp` and `index.html`
- **Signaling WebSocket URL**: default `ws://localhost:8765`
- **Video pipeline**: can be changed in sender GStreamer launch string (e.g., `videotestsrc`, `rtspsrc`)

---

## 🧪 Example GStreamer Pipeline

```cpp
gst_parse_launch("videotestsrc ! x264enc tune=zerolatency ! rtph264pay config-interval=1 pt=96 ! appsink", nullptr);
```

Or using RTSP camera input:

```cpp
gst_parse_launch("rtspsrc location=rtsp://... ! rtph264depay ! appsink", nullptr);
```

---

## 📈 Features

- ✅ Low-latency video streaming to browser
- ✅ Full WebRTC offer/answer exchange
- ✅ C++ to browser P2P

---



---

## 🙏 Acknowledgements

This project relies on the following awesome tools:

- 🎞️ [GStreamer](https://gstreamer.freedesktop.org/) – Real-time media processing and streaming framework
- 📡 [libdatachannel](https://github.com/paullouisageneau/libdatachannel) – C++ WebRTC implementation
- 🌐 [WebRTC](https://webrtc.org/) – Open standard for real-time media in browsers
- 🔌 [websocketpp](https://github.com/zaphoyd/websocketpp) – C++ WebSocket library
- ⚙️ [Boost](https://www.boost.org/) – Modern C++ utilities
- 🐍 [Python](https://www.python.org/) – For the signaling server
- 🧪 [MDN WebRTC Docs](https://developer.mozilla.org/en-US/docs/Web/API/WebRTC_API) – Reference for browser-side WebRTC