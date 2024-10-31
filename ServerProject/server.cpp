#include <iostream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <sstream>
#include <string>
#include "sqlite3.h"
#include <thread>
#include <mutex>

#pragma comment(lib, "Ws2_32.lib")

#define SERVER_PORT 5432
#define MAX_PENDING 5
#define MAX_LINE 1024
#define PRICE_PER_CARD 50.0

SOCKET serverSocket;
std::mutex db_mutex;

int initializeDatabase(sqlite3*& db) {
    const char* dbFileName = "pokemon_store.db";
    int exit = sqlite3_open(dbFileName, &db);
    if (exit) {
        std::cerr << "Error opening database: " << sqlite3_errmsg(db) << std::endl;
        return exit;
    }
    std::cout << "Database opened successfully." << std::endl;

    const char* createUsersTableSQL =
        "CREATE TABLE IF NOT EXISTS Users ("
        "ID INTEGER PRIMARY KEY AUTOINCREMENT, "
        "username TEXT, "
        "password TEXT, "
        "usd_balance DOUBLE NOT NULL, "
        "logged_in INTEGER NOT NULL DEFAULT 0, "
        "is_root INTEGER NOT NULL DEFAULT 0);";

    if (sqlite3_exec(db, createUsersTableSQL, 0, 0, 0) != SQLITE_OK) {
        std::cerr << "Error creating Users table: " << sqlite3_errmsg(db) << std::endl;
    }

    const char* createPokemonTableSQL =
        "CREATE TABLE IF NOT EXISTS Pokemon_Cards ("
        "ID INTEGER PRIMARY KEY AUTOINCREMENT, "
        "card_name TEXT NOT NULL, "
        "card_type TEXT NOT NULL, "
        "rarity TEXT NOT NULL, "
        "count INTEGER, "
        "owner_id INTEGER, "
        "FOREIGN KEY(owner_id) REFERENCES Users(ID));";

    if (sqlite3_exec(db, createPokemonTableSQL, 0, 0, 0) != SQLITE_OK) {
        std::cerr << "Error creating Pokemon_Cards table: " << sqlite3_errmsg(db) << std::endl;
    }

    std::cout << "Tables created successfully." << std::endl;
    return 0;
}

void handleClient(SOCKET clientSocket, sqlite3* db, int& currentUserId) {
    bool connected = true;
    char buffer[MAX_LINE];
    while (connected) {
        int bytesReceived = recv(clientSocket, buffer, MAX_LINE, 0);
        if (bytesReceived > 0) {
            buffer[bytesReceived] = '\0';
            std::string command(buffer);
            std::istringstream iss(command);
            std::string action;
            iss >> action;

            std::lock_guard<std::mutex> lock(db_mutex); // Lock to avoid concurrent issues

            if (action == "LOGIN") {
                std::string username, password;
                iss >> username >> password;

                std::string query = "SELECT ID FROM Users WHERE username = ? AND password = ?";
                sqlite3_stmt* stmt;
                if (sqlite3_prepare_v2(db, query.c_str(), -1, &stmt, 0) == SQLITE_OK) {
                    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_STATIC);
                    sqlite3_bind_text(stmt, 2, password.c_str(), -1, SQLITE_STATIC);

                    if (sqlite3_step(stmt) == SQLITE_ROW) {
                        currentUserId = sqlite3_column_int(stmt, 0);
                        sqlite3_finalize(stmt);

                        std::string updateLoginStatus = "UPDATE Users SET logged_in = 1 WHERE ID = ?";
                        if (sqlite3_prepare_v2(db, updateLoginStatus.c_str(), -1, &stmt, 0) == SQLITE_OK) {
                            sqlite3_bind_int(stmt, 1, currentUserId);
                            sqlite3_step(stmt);
                            sqlite3_finalize(stmt);
                            send(clientSocket, "200 OK - Login successful\n", strlen("200 OK - Login successful\n"), 0);
                        }
                    }
                    else {
                        sqlite3_finalize(stmt);
                        send(clientSocket, "401 Unauthorized - Invalid credentials\n", strlen("401 Unauthorized - Invalid credentials\n"), 0);
                    }
                }
                else {
                    send(clientSocket, "400 Database error\n", strlen("400 Database error\n"), 0);
                }
            }
            else if (action == "BALANCE") {
                std::string query = "SELECT usd_balance FROM Users WHERE ID = ?";
                sqlite3_stmt* stmt;
                if (sqlite3_prepare_v2(db, query.c_str(), -1, &stmt, 0) == SQLITE_OK) {
                    sqlite3_bind_int(stmt, 1, currentUserId);
                    if (sqlite3_step(stmt) == SQLITE_ROW) {
                        double balance = sqlite3_column_double(stmt, 0);
                        std::ostringstream response;
                        response << "200 OK - Your balance is " << balance << "\n";
                        send(clientSocket, response.str().c_str(), response.str().length(), 0);
                    }
                    sqlite3_finalize(stmt);
                }
                else {
                    send(clientSocket, "400 Database error\n", strlen("400 Database error\n"), 0);
                }
            }
            else if (action == "DEPOSIT") {
                double amount;
                iss >> amount;
                std::string query = "UPDATE Users SET usd_balance = usd_balance + ? WHERE ID = ?";
                sqlite3_stmt* stmt;
                if (sqlite3_prepare_v2(db, query.c_str(), -1, &stmt, 0) == SQLITE_OK) {
                    sqlite3_bind_double(stmt, 1, amount);
                    sqlite3_bind_int(stmt, 2, currentUserId);
                    sqlite3_step(stmt);
                    sqlite3_finalize(stmt);
                    send(clientSocket, "200 OK - Deposit successful\n", strlen("200 OK - Deposit successful\n"), 0);
                }
                else {
                    send(clientSocket, "400 Database error\n", strlen("400 Database error\n"), 0);
                }
            }
            else if (action == "LIST") {
                std::string query = "SELECT card_name, card_type, rarity, count FROM Pokemon_Cards WHERE owner_id IS NULL";
                sqlite3_stmt* stmt;
                if (sqlite3_prepare_v2(db, query.c_str(), -1, &stmt, 0) == SQLITE_OK) {
                    std::ostringstream response;
                    response << "200 OK - Available Pokémon cards:\n";
                    while (sqlite3_step(stmt) == SQLITE_ROW) {
                        response << reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)) << " | "
                            << "Type: " << reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)) << " | "
                            << "Rarity: " << reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)) << " | "
                            << "Count: " << sqlite3_column_int(stmt, 3) << "\n";
                    }
                    sqlite3_finalize(stmt);
                    send(clientSocket, response.str().c_str(), response.str().length(), 0);
                }
                else {
                    send(clientSocket, "400 Database error\n", strlen("400 Database error\n"), 0);
                }
            }
            else if (action == "LOOKUP") {
                std::string cardName;
                iss >> cardName;
                std::string query = "SELECT card_name, card_type, rarity, count FROM Pokemon_Cards WHERE card_name = ?";
                sqlite3_stmt* stmt;
                if (sqlite3_prepare_v2(db, query.c_str(), -1, &stmt, 0) == SQLITE_OK) {
                    sqlite3_bind_text(stmt, 1, cardName.c_str(), -1, SQLITE_STATIC);
                    if (sqlite3_step(stmt) == SQLITE_ROW) {
                        std::ostringstream response;
                        response << "200 OK - Card details:\n"
                            << "Name: " << reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)) << "\n"
                            << "Type: " << reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)) << "\n"
                            << "Rarity: " << reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)) << "\n"
                            << "Count: " << sqlite3_column_int(stmt, 3) << "\n";
                        send(clientSocket, response.str().c_str(), response.str().length(), 0);
                    }
                    else {
                        send(clientSocket, "404 Not Found - Card not found\n", strlen("404 Not Found - Card not found\n"), 0);
                    }
                    sqlite3_finalize(stmt);
                }
                else {
                    send(clientSocket, "400 Database error\n", strlen("400 Database error\n"), 0);
                }
            }
            else if (action == "BUY") {
                std::string cardName;
                int quantity;
                iss >> cardName >> quantity;

                double totalCost = quantity * PRICE_PER_CARD;

                // Check if the user has enough balance and if there are enough cards in stock
                std::string queryCheck = "SELECT count FROM Pokemon_Cards WHERE card_name = ? AND owner_id IS NULL";
                sqlite3_stmt* stmt;
                if (sqlite3_prepare_v2(db, queryCheck.c_str(), -1, &stmt, 0) == SQLITE_OK) {
                    sqlite3_bind_text(stmt, 1, cardName.c_str(), -1, SQLITE_STATIC);
                    if (sqlite3_step(stmt) == SQLITE_ROW) {
                        int stockCount = sqlite3_column_int(stmt, 0);
                        sqlite3_finalize(stmt);

                        if (stockCount >= quantity) {
                            // Check user's balance
                            std::string balanceQuery = "SELECT usd_balance FROM Users WHERE ID = ?";
                            sqlite3_prepare_v2(db, balanceQuery.c_str(), -1, &stmt, 0);
                            sqlite3_bind_int(stmt, 1, currentUserId);

                            if (sqlite3_step(stmt) == SQLITE_ROW) {
                                double userBalance = sqlite3_column_double(stmt, 0);
                                sqlite3_finalize(stmt);

                                if (userBalance >= totalCost) {
                                    // Proceed with the transaction
                                    // Update Pokemon stock
                                    std::string updateStockSQL = "UPDATE Pokemon_Cards SET count = count - ? WHERE card_name = ? AND owner_id IS NULL";
                                    sqlite3_prepare_v2(db, updateStockSQL.c_str(), -1, &stmt, 0);
                                    sqlite3_bind_int(stmt, 1, quantity);
                                    sqlite3_bind_text(stmt, 2, cardName.c_str(), -1, SQLITE_STATIC);
                                    sqlite3_step(stmt);
                                    sqlite3_finalize(stmt);

                                    // Assign ownership of the purchased cards to the user
                                    std::string assignOwnershipSQL = "INSERT INTO Pokemon_Cards (card_name, card_type, rarity, count, owner_id) "
                                        "VALUES (?, 'Unknown', 'Unknown', ?, ?)";
                                    sqlite3_prepare_v2(db, assignOwnershipSQL.c_str(), -1, &stmt, 0);
                                    sqlite3_bind_text(stmt, 1, cardName.c_str(), -1, SQLITE_STATIC);
                                    sqlite3_bind_int(stmt, 2, quantity);
                                    sqlite3_bind_int(stmt, 3, currentUserId);
                                    sqlite3_step(stmt);
                                    sqlite3_finalize(stmt);

                                    // Update user's balance
                                    std::string updateBalanceSQL = "UPDATE Users SET usd_balance = usd_balance - ? WHERE ID = ?";
                                    sqlite3_prepare_v2(db, updateBalanceSQL.c_str(), -1, &stmt, 0);
                                    sqlite3_bind_double(stmt, 1, totalCost);
                                    sqlite3_bind_int(stmt, 2, currentUserId);
                                    sqlite3_step(stmt);
                                    sqlite3_finalize(stmt);

                                    send(clientSocket, "200 OK - Purchase successful\n", strlen("200 OK - Purchase successful\n"), 0);
                                }
                                else {
                                    send(clientSocket, "400 Insufficient funds\n", strlen("400 Insufficient funds\n"), 0);
                                }
                            }
                            else {
                                send(clientSocket, "400 Database error\n", strlen("400 Database error\n"), 0);
                            }
                        }
                        else {
                            send(clientSocket, "400 Not enough stock\n", strlen("400 Not enough stock\n"), 0);
                        }
                    }
                    else {
                        sqlite3_finalize(stmt);
                        send(clientSocket, "404 Card not found\n", strlen("404 Card not found\n"), 0);
                    }
                }
                else {
                    send(clientSocket, "400 Database error\n", strlen("400 Database error\n"), 0);
                }
            }
            else if (action == "SELL") {
                std::string cardName;
                int quantity;
                iss >> cardName >> quantity;
                double totalEarnings = quantity * PRICE_PER_CARD;

                std::string queryCheck = "SELECT count FROM Pokemon_Cards WHERE card_name = ? AND owner_id = ?";
                sqlite3_stmt* stmt;
                if (sqlite3_prepare_v2(db, queryCheck.c_str(), -1, &stmt, 0) == SQLITE_OK) {
                    sqlite3_bind_text(stmt, 1, cardName.c_str(), -1, SQLITE_STATIC);
                    sqlite3_bind_int(stmt, 2, currentUserId);

                    if (sqlite3_step(stmt) == SQLITE_ROW) {
                        int stockCount = sqlite3_column_int(stmt, 0);

                        if (stockCount >= quantity) {
                            sqlite3_finalize(stmt);

                            std::string updateStockSQL = "UPDATE Pokemon_Cards SET count = count - ? WHERE card_name = ? AND owner_id = ?";
                            std::string updateBalanceSQL = "UPDATE Users SET usd_balance = usd_balance + ? WHERE ID = ?";
                            sqlite3_stmt* updateStmt;

                            if (sqlite3_prepare_v2(db, updateStockSQL.c_str(), -1, &updateStmt, 0) == SQLITE_OK) {
                                sqlite3_bind_int(updateStmt, 1, quantity);
                                sqlite3_bind_text(updateStmt, 2, cardName.c_str(), -1, SQLITE_STATIC);
                                sqlite3_bind_int(updateStmt, 3, currentUserId);
                                sqlite3_step(updateStmt);
                                sqlite3_finalize(updateStmt);
                            }

                            if (sqlite3_prepare_v2(db, updateBalanceSQL.c_str(), -1, &updateStmt, 0) == SQLITE_OK) {
                                sqlite3_bind_double(updateStmt, 1, totalEarnings);
                                sqlite3_bind_int(updateStmt, 2, currentUserId);
                                sqlite3_step(updateStmt);
                                sqlite3_finalize(updateStmt);
                            }

                            send(clientSocket, "200 OK - Sell successful\n", strlen("200 OK - Sell successful\n"), 0);
                        }
                        else {
                            send(clientSocket, "400 Not enough stock to sell\n", strlen("400 Not enough stock to sell\n"), 0);
                        }
                    }
                    else {
                        send(clientSocket, "404 Not Found - Card not owned by user\n", strlen("404 Not Found - Card not owned by user\n"), 0);
                        sqlite3_finalize(stmt);
                    }
                }
                else {
                    send(clientSocket, "400 Database error\n", strlen("400 Database error\n"), 0);
                }
            }
            else if (action == "WHO") {
                std::string query = "SELECT username FROM Users WHERE logged_in = 1";
                sqlite3_stmt* stmt;
                if (sqlite3_prepare_v2(db, query.c_str(), -1, &stmt, 0) == SQLITE_OK) {
                    std::ostringstream response;
                    response << "200 OK - Logged-in users:\n";
                    while (sqlite3_step(stmt) == SQLITE_ROW) {
                        response << reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)) << "\n";
                    }
                    sqlite3_finalize(stmt);
                    send(clientSocket, response.str().c_str(), response.str().length(), 0);
                }
                else {
                    send(clientSocket, "400 Database error\n", strlen("400 Database error\n"), 0);
                }
            }
            else if (action == "LOGOUT") {
                std::string query = "UPDATE Users SET logged_in = 0 WHERE ID = ?";
                sqlite3_stmt* stmt;
                if (sqlite3_prepare_v2(db, query.c_str(), -1, &stmt, 0) == SQLITE_OK) {
                    sqlite3_bind_int(stmt, 1, currentUserId);
                    sqlite3_step(stmt);
                    sqlite3_finalize(stmt);
                    send(clientSocket, "200 OK - Logged out\n", strlen("200 OK - Logged out\n"), 0);
                    currentUserId = -1;
                }
                else {
                    send(clientSocket, "400 Database error\n", strlen("400 Database error\n"), 0);
                }
            }
            else if (action == "QUIT") {
                send(clientSocket, "200 OK - Quitting\n", strlen("200 OK - Quitting\n"), 0);
                connected = false;
            }
            else if (action == "SHUTDOWN") {
                send(clientSocket, "200 OK - Server shutting down\n", strlen("200 OK - Server shutting down\n"), 0);
                closesocket(clientSocket);
                closesocket(serverSocket);
                sqlite3_close(db);
                WSACleanup();
                return;
            }
            else {
                send(clientSocket, "400 Unknown command\n", strlen("400 Unknown command\n"), 0);
            }
        }
    }
    closesocket(clientSocket);
}

int main() {
    WSADATA wsa;
    sockaddr_in serverAddr, clientAddr;
    sqlite3* db = nullptr;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "Failed to initialize Winsock. Error: " << WSAGetLastError() << std::endl;
        return 1;
    }

    if (initializeDatabase(db) != 0) {
        WSACleanup();
        return 1;
    }

    serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket == INVALID_SOCKET) {
        std::cerr << "Could not create socket: " << WSAGetLastError() << std::endl;
        sqlite3_close(db);
        WSACleanup();
        return 1;
    }

    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(SERVER_PORT);

    if (bind(serverSocket, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        std::cerr << "Bind failed: " << WSAGetLastError() << std::endl;
        closesocket(serverSocket);
        sqlite3_close(db);
        WSACleanup();
        return 1;
    }

    if (listen(serverSocket, MAX_PENDING) == SOCKET_ERROR) {
        std::cerr << "Listen failed: " << WSAGetLastError() << std::endl;
        closesocket(serverSocket);
        sqlite3_close(db);
        WSACleanup();
        return 1;
    }

    std::cout << "Server is listening on port " << SERVER_PORT << "..." << std::endl;

    int clientAddrLen = sizeof(clientAddr);
    while (true) {
        SOCKET clientSocket = accept(serverSocket, (struct sockaddr*)&clientAddr, &clientAddrLen);
        if (clientSocket == INVALID_SOCKET) {
            std::cerr << "Accept failed: " << WSAGetLastError() << std::endl;
            continue;
        }

        int currentUserId = -1;
        std::thread clientThread(handleClient, clientSocket, db, std::ref(currentUserId));
        clientThread.detach();
    }

    closesocket(serverSocket);
    sqlite3_close(db);
    WSACleanup();
    return 0;
}
