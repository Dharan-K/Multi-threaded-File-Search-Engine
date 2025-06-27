
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <filesystem>
#include <chrono>
#include <atomic>
#include <queue>
#include <condition_variable>
#include <unordered_map>

namespace fs = std::filesystem;

class ThreadPool {
private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable condition;
    std::atomic<bool> stop;

public:
    ThreadPool(size_t threads) : stop(false) {
        for (size_t i = 0; i < threads; ++i)
            workers.emplace_back([this] {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(this->queue_mutex);
                        this->condition.wait(lock, [this] { 
                            return this->stop || !this->tasks.empty(); 
                        });
                        if (this->stop && this->tasks.empty())
                            return;
                        task = std::move(this->tasks.front());
                        this->tasks.pop();
                    }
                    task();
                }
            });
    }

    template<class F>
    void enqueue(F&& f) {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            tasks.emplace(std::forward<F>(f));
        }
        condition.notify_one();
    }

    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            stop = true;
        }
        condition.notify_all();
        for (std::thread &worker : workers)
            worker.join();
    }
};

class SearchEngine {
private:
    std::mutex results_mutex;
    std::vector<std::pair<std::string, std::vector<int>>> results;
    std::atomic<int> files_processed{0};
    std::atomic<int> total_files{0};

    bool searchInFile(const std::string& filepath, const std::string& searchTerm) {
        std::ifstream file(filepath);
        if (!file.is_open()) return false;

        std::string line;
        int lineNumber = 0;
        std::vector<int> matchingLines;

        while (std::getline(file, line)) {
            lineNumber++;
            if (line.find(searchTerm) != std::string::npos) {
                matchingLines.push_back(lineNumber);
            }
        }

        if (!matchingLines.empty()) {
            std::lock_guard<std::mutex> lock(results_mutex);
            results.push_back({filepath, matchingLines});
            return true;
        }
        return false;
    }

public:
    void search(const std::string& rootPath, const std::string& searchTerm) {
        // Count total files first
        for (const auto& entry : fs::recursive_directory_iterator(rootPath)) {
            if (fs::is_regular_file(entry)) {
                total_files++;
            }
        }

        // Create a thread pool with hardware concurrency
        ThreadPool pool(std::thread::hardware_concurrency());
        
        auto start = std::chrono::high_resolution_clock::now();

        // Process files using thread pool
        for (const auto& entry : fs::recursive_directory_iterator(rootPath)) {
            if (fs::is_regular_file(entry)) {
                pool.enqueue([this, entry, searchTerm] {
                    searchInFile(entry.path().string(), searchTerm);
                    files_processed++;
                    
                    // Print progress
                    float progress = static_cast<float>(files_processed) / total_files * 100;
                    std::cout << "\rProgress: " << std::fixed << std::setprecision(1) 
                              << progress << "% (" << files_processed << "/" << total_files 
                              << " files)" << std::flush;
                });
            }
        }
        
        // Wait for all tasks to complete (ThreadPool destructor handles this)
        while (files_processed < total_files) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> elapsed = end - start;
        
        std::cout << "\nSearch completed in " << elapsed.count() << " seconds." << std::endl;
        std::cout << "Found " << results.size() << " files containing the search term." << std::endl;
        
        // Display results
        if (!results.empty()) {
            std::cout << "\nResults:\n";
            for (const auto& result : results) {
                std::cout << "File: " << result.first << std::endl;
                std::cout << "  Matching lines: ";
                for (size_t i = 0; i < result.second.size(); ++i) {
                    std::cout << result.second[i];
                    if (i < result.second.size() - 1) std::cout << ", ";
                }
                std::cout << std::endl;
            }
        }
    }
};

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cout << "Usage: " << argv[0] << " <directory_path> <search_term>" << std::endl;
        return 1;
    }

    std::string directoryPath = argv[1];
    std::string searchTerm = argv[2];

    try {
        if (!fs::exists(directoryPath)) {
            std::cerr << "Directory does not exist." << std::endl;
            return 1;
        }

        std::cout << "Searching for '" << searchTerm << "' in " << directoryPath << std::endl;
        
        SearchEngine engine;
        engine.search(directoryPath, searchTerm);
        
    } catch (const fs::filesystem_error& e) {
        std::cerr << "Filesystem error: " << e.what() << std::endl;
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
