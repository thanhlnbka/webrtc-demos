#include <rtc/rtc.hpp>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <websocketpp/config/asio_no_tls_client.hpp>
#include <websocketpp/client.hpp>
#include <nlohmann/json.hpp>
#include <iostream>
#include <thread>
#include <string>
#include <stdexcept>
#include <atomic>
#include <map>
#include <mutex>

using json = nlohmann::json;
using ws_client = websocketpp::client<websocketpp::config::asio_client>;


class PeerConnectionManager {
public:
    struct PeerInfo {
        std::shared_ptr<rtc::PeerConnection> pc;
        std::shared_ptr<rtc::Track> videoTrack;
        std::string sessionId;
        std::atomic<bool> trackOpen{false};
    };

    PeerConnectionManager() {}

    std::shared_ptr<PeerInfo> createPeerConnection(const std::string& viewerId, const std::string& sessionId) {
        std::lock_guard<std::mutex> lock(mutex);

        auto it = connections.find(viewerId);
        if (it != connections.end()) {
            if (it->second->pc) {
                it->second->pc->close();
            }
            connections.erase(it);
        }

        auto peerInfo = std::make_shared<PeerInfo>();
        peerInfo->sessionId = sessionId;

        rtc::Configuration config;
        config.iceServers.emplace_back("stun:stun.l.google.com:19302");
        config.disableAutoNegotiation = true;


        peerInfo->pc = std::make_shared<rtc::PeerConnection>(config);

        rtc::Description::Video media("video", rtc::Description::Direction::SendOnly);
        media.addH264Codec(96);
        peerInfo->videoTrack = peerInfo->pc->addTrack(media);

       
        std::weak_ptr<PeerInfo> weakInfo = peerInfo;

        peerInfo->videoTrack->onOpen([weakInfo]() {
            if (auto shared = weakInfo.lock()) {
                shared->trackOpen = true;
                std::cout << "Video track is now open for viewer" << std::endl;
            }
        });

        peerInfo->videoTrack->onClosed([weakInfo]() {
            if (auto shared = weakInfo.lock()) {
                shared->trackOpen = false;
                std::cout << "Video track is now closed for viewer" << std::endl;
            }
        });

        connections[viewerId] = peerInfo;

        return peerInfo;
    }

    std::shared_ptr<PeerInfo> getPeerInfo(const std::string& viewerId) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = connections.find(viewerId);
        if (it != connections.end()) {
            return it->second;
        }
        return nullptr;
    }

    
    void removePeerConnection(const std::string& viewerId) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = connections.find(viewerId);
        if (it != connections.end()) {
            if (it->second->pc) {
                it->second->pc->close();
            }
            connections.erase(it);
            std::cout << "Removed peer connection for viewer: " << viewerId << std::endl;
        }
    }

    void sendVideoFrameToAll(const std::vector<std::byte>& data) {
        std::vector<std::shared_ptr<PeerInfo>> peers;
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (auto& [_, peerInfo] : connections) {
                peers.push_back(peerInfo);
            }
        }

        for (auto& peerInfo : peers) {
            if (peerInfo && peerInfo->videoTrack && peerInfo->trackOpen &&
                peerInfo->pc->state() == rtc::PeerConnection::State::Connected) {
                try {
                    peerInfo->videoTrack->send(data);
                } catch (const std::exception& e) {
                    std::cerr << "Error sending video: " << e.what() << std::endl;
                }
            }
        }
    }

    std::vector<std::string> getAllViewerIds() {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<std::string> ids;
        for (const auto& [viewerId, _] : connections) {
            ids.push_back(viewerId);
        }
        return ids;
    }

    size_t getConnectionCount() {
        std::lock_guard<std::mutex> lock(mutex);
        return connections.size();
    }


    void closeAll() {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& [_, peerInfo] : connections) {
            if (peerInfo->pc) {
                peerInfo->pc->close();
            }
        }
        connections.clear();
    }

private:
    std::map<std::string, std::shared_ptr<PeerInfo>> connections;
    std::mutex mutex;
};



PeerConnectionManager peerConnectionManager;
ws_client client;
websocketpp::connection_hdl ws_hdl;
std::atomic<bool> ws_connected{false};
std::string localClientId; // ID của client này (sender)

static GstFlowReturn on_new_sample(GstElement *appsink, gpointer) {
    GstSample *sample = gst_app_sink_pull_sample(GST_APP_SINK(appsink));
    if (!sample) {
        std::cerr << "Error: No sample received from appsink" << std::endl;
        return GST_FLOW_ERROR;
    }
    
    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstMapInfo map;
    if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        try {
            // std::cout << "Sending " << map.size << " bytes of video data to all peers" << std::endl;
            std::vector<std::byte> message(reinterpret_cast<std::byte*>(map.data),
                                           reinterpret_cast<std::byte*>(map.data + map.size));
            
            // Gửi video frame tới tất cả kết nối
            peerConnectionManager.sendVideoFrameToAll(message);
            
        } catch (const std::exception &e) {
            std::cerr << "Error sending video data: " << e.what() << std::endl;
        }
        gst_buffer_unmap(buffer, &map);
    } else {
        std::cerr << "Error: Unable to map buffer" << std::endl;
    }
    
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

void create_offer_for_viewer(const std::string& viewerId, const std::string& sessionId) {
    std::cout << "Creating new offer for viewer: " << viewerId << " with session: " << sessionId << std::endl;
    
    auto peerInfo = peerConnectionManager.createPeerConnection(viewerId, sessionId);
    
    // SEND SDP -> SIGNALING
    peerInfo->pc->onLocalDescription([viewerId, sessionId](rtc::Description desc) {
        try {
            if (!ws_connected) {
                std::cerr << "WebSocket not connected, can't send local description" << std::endl;
                return;
            }

            json j = {
                {"type", desc.typeString()},
                {"sdp", std::string(desc)},
                {"target_id", viewerId}, 
                {"session_id", sessionId}
            };

            std::cout << "Sending local description to viewer " << viewerId << std::endl;
            client.send(ws_hdl, j.dump(), websocketpp::frame::opcode::text);
        } catch (const std::exception &e) {
            std::cerr << "Error sending local description: " << e.what() << std::endl;
        }
    });

    // SEND ICE candidate
    peerInfo->pc->onLocalCandidate([viewerId, sessionId](rtc::Candidate cand) {
        try {
            if (!ws_connected) {
                std::cerr << "WebSocket not connected, can't send candidate" << std::endl;
                return;
            }

            std::string candidateStr = std::string(cand);
            int mLineIndex = 0;
            std::string sdpMid = "video";

            json j = {
                {"candidate", candidateStr},
                {"sdpMLineIndex", mLineIndex},
                {"sdpMid", sdpMid},
                {"target_id", viewerId},  
                {"session_id", sessionId} 
            };

            std::cout << "Sending local ICE candidate to viewer " << viewerId << std::endl;
            client.send(ws_hdl, j.dump(), websocketpp::frame::opcode::text);
        } catch (const std::exception &e) {
            std::cerr << "Error sending candidate: " << e.what() << std::endl;
        }
    });

    // Handle State Connection for PeerConnection
    peerInfo->pc->onStateChange([viewerId](rtc::PeerConnection::State state) {
        std::cout << "PeerConnection state for " << viewerId << ": ";
        switch (state) {
            case rtc::PeerConnection::State::New: std::cout << "New"; break;
            case rtc::PeerConnection::State::Connecting: std::cout << "Connecting"; break;
            case rtc::PeerConnection::State::Connected: std::cout << "Connected"; break;
            case rtc::PeerConnection::State::Disconnected: std::cout << "Disconnected";  break;
            case rtc::PeerConnection::State::Failed: std::cout << "Failed"; break;
            case rtc::PeerConnection::State::Closed: std::cout << "Closed"; break;
            default: std::cout << "Unknown"; break;
        }
        std::cout << std::endl;

        std::cout << "Num connection count before remove: "
              << peerConnectionManager.getConnectionCount() << std::endl;
        
        // auto ids = peerConnectionManager.getAllViewerIds();
        // std::cout << "👥 Viewers current: ";
        // for (const auto& id : ids) {
        //     std::cout << id << " ";
        // }
        // std::cout << std::endl;

        // if (state == rtc::PeerConnection::State::Disconnected || 
        //     state == rtc::PeerConnection::State::Failed || 
        //     state == rtc::PeerConnection::State::Closed) {
        //     std::cout << "Remove peerconnection ...." << std::endl;
        //     peerConnectionManager.removePeerConnection(viewerId);
        // }

        // std::cout << "Num connection count after remove: "
        //       << peerConnectionManager.getConnectionCount() << std::endl;

    });
    // Handale State Gathering for PeerCOnnection
    peerInfo->pc->onGatheringStateChange([](rtc::PeerConnection::GatheringState gatheringState) {
        std::cout << "Gathering state changed: ";
        switch (gatheringState) {
            case rtc::PeerConnection::GatheringState::New:
                std::cout << "New";
                break;
            case rtc::PeerConnection::GatheringState::InProgress:
                std::cout << "InProgress";
                break;
            case rtc::PeerConnection::GatheringState::Complete:
                std::cout << "Complete";
                break;
        }
        std::cout << std::endl;
    });
    
    // Make Offer 
    peerInfo->pc->setLocalDescription();
}

void start_signaling(const std::string& ws_url) {
    try {
        client.init_asio();
        client.clear_access_channels(websocketpp::log::alevel::all);
        client.set_access_channels(websocketpp::log::alevel::connect | 
                                  websocketpp::log::alevel::disconnect | 
                                  websocketpp::log::alevel::app);
        
        client.set_open_handler([](websocketpp::connection_hdl hdl) {
            ws_hdl = hdl;
            ws_connected = true;
            std::cout << "Connected to signaling server" << std::endl;
            try {
                json client_info = {
                    {"client_type", "sender"}  
                };
                std::string message = client_info.dump();
                client.send(hdl, message, websocketpp::frame::opcode::text);
            } catch (const std::exception& e) {
                std::cerr << "Error sending client info: " << e.what() << std::endl;
            }
        });
        
        client.set_close_handler([](websocketpp::connection_hdl) {
            ws_connected = false;
            std::cout << "Disconnected from signaling server" << std::endl;
        });
        
        client.set_fail_handler([](websocketpp::connection_hdl) {
            ws_connected = false;
            std::cerr << "Failed to connect to signaling server" << std::endl;
        });
        
        client.set_message_handler([](websocketpp::connection_hdl, ws_client::message_ptr msg) {
            try {
                auto data = msg->get_payload();
                std::cout << "Received message: " << data << std::endl;
                auto j = json::parse(data);

                if (j.contains("type") && j["type"] == "registration_successful") {
                    if (j.contains("client_id")) {
                        localClientId = j["client_id"];
                        std::cout << "Registration successful. Local client ID: " << localClientId << std::endl;
                    }
                    return;
                }
                
                // Process requests to create new offers when new viewers connect
                if (j.contains("type") && j["type"] == "create_new_offer") {
                    std::string viewerId = j["viewer_id"];
                    std::string sessionId = j.contains("session_id") ? j["session_id"].get<std::string>() : std::to_string(rand());

                    create_offer_for_viewer(viewerId, sessionId);
                    return;
                }
                
                // SDP processing
                if (j.contains("sdp")) {
                    std::string sdp = j["sdp"];
                    std::string type = j["type"];
                    std::string viewerId = j.contains("target_id") ? j["target_id"] : 
                                        (j.contains("from") ? j["from"] : "unknown");
                    std::string sessionId = j.contains("session_id") ? j["session_id"] : "default";
                    
                    std::cout << "Received SDP " << type << " from " << viewerId << " session " << sessionId << std::endl;
                    
                    // If it is an answer, need to update remote description for specific viewer
                    if (type == "answer") {
                        auto peerInfo = peerConnectionManager.getPeerInfo(viewerId);
                        if (peerInfo && peerInfo->pc) {
                            try {
                                peerInfo->pc->setRemoteDescription(rtc::Description(sdp, type));
                            } catch (const std::exception& e) {
                                std::cerr << "Error setting remote description: " << e.what() << std::endl;
                            }
                        } else {
                            std::cerr << "No peer connection found for viewer: " << viewerId << std::endl;
                            // If no connection found, create a new one
                            create_offer_for_viewer(viewerId, sessionId);
                        }
                    }
                } 
                // ICE candidate processing
                else if (j.contains("candidate")) {
                    std::string candidate = j["candidate"];
                    std::string viewerId = j.contains("target_id") ? j["target_id"] : 
                                        (j.contains("from") ? j["from"] : "unknown");
                    
                    std::cout << "Received ICE candidate from " << viewerId << std::endl;
                    
                    auto peerInfo = peerConnectionManager.getPeerInfo(viewerId);
                    if (peerInfo && peerInfo->pc) {
                        try {
                            peerInfo->pc->addRemoteCandidate(rtc::Candidate(candidate));
                        } catch (const std::exception& e) {
                            std::cerr << "Error adding remote candidate: " << e.what() << std::endl;
                        }
                    } else {
                        std::cerr << "No peer connection found for viewer: " << viewerId << std::endl;
                    }
                }
                // Handle session join requests from new viewers
                else if (j.contains("type") && j["type"] == "viewer_joined") {
                    std::string viewerId = j["viewer_id"];
                    std::string sessionId = j.contains("session_id") ? j["session_id"].get<std::string>() : std::to_string(rand());

                    
                    std::cout << "New viewer joined: " << viewerId << ", creating offer..." << std::endl;
                    create_offer_for_viewer(viewerId, sessionId);
                }
            } catch (const std::exception &e) {
                std::cerr << "Error handling WebSocket message: " << e.what() << std::endl;
            }
        });
        
        websocketpp::lib::error_code ec;
        auto con = client.get_connection(ws_url, ec);
        if (ec) {
            std::cerr << "WebSocket connection error: " << ec.message() << std::endl;
            return;
        }
        
        client.connect(con);
        std::thread ws_thread([&]() { 
            try {
                client.run();
            } catch (const std::exception& e) {
                std::cerr << "WebSocket thread error: " << e.what() << std::endl;
            }
        });
        ws_thread.detach();
        
    } catch (const std::exception &e) {
        std::cerr << "Error initializing WebSocket client: " << e.what() << std::endl;
    }
}

GstElement* setup_gstreamer_pipeline(const std::string& video_path) {
    std::string pipeline_str =
    "filesrc location=" + video_path + " ! "
    "qtdemux ! avdec_h264 ! videoconvert ! x264enc tune=zerolatency bitrate=1000 ! "
    "video/x-h264,profile=baseline,stream-format=byte-stream ! "
    "rtph264pay pt=96 config-interval=-1 ! "
    "appsink name=appsink";
    
    std::cout << "Using GStreamer pipeline: " << pipeline_str << std::endl;
    
    GError *error = nullptr;
    GstElement *pipeline = gst_parse_launch(pipeline_str.c_str(), &error);
    
    if (!pipeline) {
        std::string error_msg = error ? error->message : "Unknown error";
        g_clear_error(&error);
        throw std::runtime_error("Failed to create GStreamer pipeline: " + error_msg);
    }
    
    GstElement *appsink = gst_bin_get_by_name(GST_BIN(pipeline), "appsink");
    if (!appsink) {
        gst_object_unref(pipeline);
        throw std::runtime_error("Failed to find appsink element in pipeline");
    }
    
    g_object_set(appsink, "emit-signals", TRUE, "drop", TRUE, "max-buffers", 1, NULL);
    g_signal_connect(appsink, "new-sample", G_CALLBACK(on_new_sample), nullptr);
    
    gst_object_unref(appsink);
    return pipeline;
}

int main(int argc, char *argv[]) {
    try {
        if (argc < 2) {
            std::cerr << "Usage: " << argv[0] << " <video_file_path> [signaling_url]" << std::endl;
            std::cerr << "Example: " << argv[0] << " ../video.mp4 ws://localhost:8765" << std::endl;
            return 1;
        }
        
        std::string video_path = argv[1];
        std::string signaling_url = (argc > 2) ? argv[2] : "ws://localhost:8765";
        
        // Initialize libraries
        gst_init(&argc, &argv);
        rtc::InitLogger(rtc::LogLevel::Info);
        
        // Start signaling
        std::cout << "Connecting to signaling server at " << signaling_url << std::endl;
        start_signaling(signaling_url);
        
        // Give some time for WebSocket connection to establish
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        if (!ws_connected) {
            std::cerr << "Warning: WebSocket connection not established yet. Continuing anyway..." << std::endl;
        }
        
        // Setup GStreamer pipeline
        GstElement *pipeline = setup_gstreamer_pipeline(video_path);
        
        // Start GStreamer pipeline
        GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
        if (ret == GST_STATE_CHANGE_FAILURE) {
            throw std::runtime_error("Failed to start GStreamer pipeline");
        }
        
        std::cout << "Streaming started. Press Ctrl+C to stop." << std::endl;
        
        // Run main loop
        GMainLoop *loop = g_main_loop_new(NULL, FALSE);
        
        g_main_loop_run(loop);
        
        // Cleanup
        std::cout << "Cleaning up resources..." << std::endl;
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        g_main_loop_unref(loop);
        
        // Close all peer connections
        peerConnectionManager.closeAll();
        
        if (ws_connected) {
            websocketpp::lib::error_code ec;
            client.close(ws_hdl, websocketpp::close::status::normal, "Finished streaming", ec);
        }
        
    } catch (const std::exception &e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
    
    std::cout << "Clean shutdown completed" << std::endl;
    return 0;
}