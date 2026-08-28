#include <boost/asio.hpp>
#include <cstdint>
#include <iostream>
#include <thread>
#include <memory>
#include <vector>
#include "edgeflow/simulator/config.hpp"
#include "edgeflow/simulator/device_client.hpp"

int main(int argc, char** argv) {
    edgeflow::simulator::Config config;
    try {
        config = edgeflow::simulator::parse_args(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "edgeflow-simulator: " << e.what() << '\n';
        return 1;
    }

    try {
        // One io_context per thread, each owning a disjoint slice of the fleet.
        // No DeviceClient is ever touched by two threads, so the clients keep
        // their single-threaded assumptions (including a non-atomic
        // events_sent_ counter).
        std::vector<std::unique_ptr<boost::asio::io_context>> contexts;
        contexts.reserve(config.thread_count);
        for (std::size_t i = 0; i < config.thread_count; ++i) {
            contexts.push_back(std::make_unique<boost::asio::io_context>());
        }

        std::vector<std::shared_ptr<edgeflow::simulator::DeviceClient>> clients;
        clients.reserve(config.device_count + config.chaos_device_spike_count);

        auto make_client = [&](std::size_t index) {
            auto& context = *contexts[index % config.thread_count];
            auto client = std::make_shared<edgeflow::simulator::DeviceClient>(
                context, config.host, config.port, static_cast<std::int64_t>(index),
                config.events_per_second_per_device, config.chaos_latency,
                config.chaos_packet_loss_percent);
            clients.push_back(client);
            client->start();
        };

        for (std::size_t i = 0; i < config.device_count; ++i) {
            make_client(i);
        }

        // Spike and stop timers live on context 0.
        boost::asio::steady_timer spike_timer(*contexts[0]);
        if (config.chaos_device_spike_count > 0) {
            spike_timer.expires_after(std::chrono::seconds(config.chaos_device_spike_at_sec));
            spike_timer.async_wait([&](const boost::system::error_code& error) {
                if (error) return;
                std::cout << "edgeflow-simulator: chaos device-spike -- adding "
                          << config.chaos_device_spike_count << " devices\n";
                for (std::size_t i = 0; i < config.chaos_device_spike_count; ++i) {
                    make_client(config.device_count + i);
                }
            });
        }

        boost::asio::steady_timer stop_timer(*contexts[0]);
        stop_timer.expires_after(std::chrono::seconds(config.duration_seconds));
        stop_timer.async_wait([&](const boost::system::error_code&) {
            for (auto& client : clients) {
                client->stop();
            }
            for (auto& context : contexts) {
                context->stop();
            }
        });

        std::cout << "edgeflow-simulator: connecting " << config.device_count
                  << " devices to " << config.host << ":" << config.port << " for "
                  << config.duration_seconds << "s across " << config.thread_count
                  << " thread(s)\n";

        std::vector<std::thread> threads;
        threads.reserve(config.thread_count);
        for (std::size_t i = 1; i < config.thread_count; ++i) {
            threads.emplace_back([&contexts, i] { contexts[i]->run(); });
        }
        contexts[0]->run();
        for (auto& thread : threads) {
            thread.join();
        }

        std::uint64_t events_sent = 0;
        for (const auto& client : clients) {
            events_sent += client->events_sent();
        }
        std::cout << "edgeflow-simulator: done (" << clients.size()
                  << " total devices, " << events_sent << " events sent)\n";
    } catch (const std::exception& e) {
        std::cerr << "edgeflow-simulator: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
