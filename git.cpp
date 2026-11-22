#include <iostream>
#include <vector>
#include <string>
#include <unordered_map>
#include <map>
#include <queue>
#include <cmath>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <memory>
#include <set>
#include <fstream>
#include <cstring>

#if defined(_WIN32) || defined(_WIN64)
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    using socket_t = SOCKET;
    #define CLOSE_SOCKET closesocket
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <unistd.h>
    #include <arpa/inet.h>
    using socket_t = int;
    #define CLOSE_SOCKET close
#endif

// ============================================================================
// DATA STRUCTURES & MODELS FOR BUDGETBEE
// ============================================================================

struct Transaction {
    int id;
    std::string type;    // "in" (Income) or "out" (Expense)
    double amount;
    std::string category;
    std::string note;
    std::string date;    // YYYY-MM-DD
};

struct Bill {
    int id;
    std::string name;
    std::string type;    // "EMI", "Bill", "Subscription"
    std::string category;
    double amount;
    int dueDay;          // 1 - 31
    bool paid;
};

struct Goal {
    int id;
    std::string name;
    double target;
    double saved;
    int priority;        // 1 to 10 (Used in Knapsack DP)
};

struct SharedExpense {
    std::string payer;
    double amount;
    std::string description;
    std::vector<std::string> participants;
};

struct DebtSettlement {
    std::string debtor;   // Person who pays
    std::string creditor; // Person who receives
    double amount;
};

// ============================================================================
// 1. TRIE (PREFIX TREE) DATA STRUCTURE
//    Used for: Instant O(L) Prefix Search & Auto-complete on notes & categories
//    Time Complexity:
//      - Insertion: O(L) where L is string length
//      - Prefix Search: O(L + M) where M is number of matching results
//    Space Complexity: O(Total characters inserted)
// ============================================================================
class TrieNode {
public:
    std::unordered_map<char, std::unique_ptr<TrieNode>> children;
    bool isEndOfWord;
    std::vector<int> transactionIds; // Stores IDs of matching transactions

    TrieNode() : isEndOfWord(false) {}
};

class TransactionTrie {
private:
    std::unique_ptr<TrieNode> root;

    std::string toLower(const std::string& str) const {
        std::string res = str;
        std::transform(res.begin(), res.end(), res.begin(), [](unsigned char c) {
            return std::tolower(c);
        });
        return res;
    }

    void collectIds(TrieNode* node, std::set<int>& resultIds) const {
        if (!node) return;
        for (int id : node->transactionIds) {
            resultIds.insert(id);
        }
        for (const auto& pair : node->children) {
            collectIds(pair.second.get(), resultIds);
        }
    }

public:
    TransactionTrie() {
        root = std::make_unique<TrieNode>();
    }

    void clear() {
        root = std::make_unique<TrieNode>();
    }

    // Insert word and associate with a transaction ID
    void insert(const std::string& word, int txId) {
        if (word.empty()) return;
        std::string lowerWord = toLower(word);
        TrieNode* current = root.get();

        for (char ch : lowerWord) {
            if (current->children.find(ch) == current->children.end()) {
                current->children[ch] = std::make_unique<TrieNode>();
            }
            current = current->children[ch].get();
            current->transactionIds.push_back(txId);
        }
        current->isEndOfWord = true;
    }

    // Query all transaction IDs matching prefix
    std::vector<int> searchPrefix(const std::string& prefix) const {
        std::string lowerPrefix = toLower(prefix);
        TrieNode* current = root.get();
        for (char ch : lowerPrefix) {
            if (current->children.find(ch) == current->children.end()) {
                return {}; // Prefix not found
            }
            current = current->children[ch].get();
        }
        std::set<int> uniqueIds;
        collectIds(current, uniqueIds);
        return std::vector<int>(uniqueIds.begin(), uniqueIds.end());
    }
};

// ============================================================================
// 2. HEAP / PRIORITY QUEUE DATA STRUCTURE
//    Used for:
//      A) Top-K Expense Extractor via Min-Heap (O(N log K) time)
//      B) Upcoming Bill Scheduler via Min-Heap (earliest due day first)
// ============================================================================
class TopKExpenseHeap {
public:
    // Custom comparator for min-heap by amount
    struct CompareTx {
        bool operator()(const Transaction* a, const Transaction* b) const {
            return a->amount > b->amount; // Smallest amount on top
        }
    };

    // Finds top K expenses in O(N log K) time and O(K) space
    static std::vector<Transaction> getTopK(const std::vector<Transaction>& transactions, int k) {
        if (k <= 0) return {};
        std::priority_queue<const Transaction*, std::vector<const Transaction*>, CompareTx> minHeap;

        for (const auto& tx : transactions) {
            if (tx.type != "out") continue; // only expenses

            if ((int)minHeap.size() < k) {
                minHeap.push(&tx);
            } else if (tx.amount > minHeap.top()->amount) {
                minHeap.pop();
                minHeap.push(&tx);
            }
        }

        std::vector<Transaction> result;
        while (!minHeap.empty()) {
            result.push_back(*minHeap.top());
            minHeap.pop();
        }
        // Result is in ascending order, reverse to get descending (largest first)
        std::reverse(result.begin(), result.end());
        return result;
    }
};

// Min-Heap for upcoming bill scheduling
struct BillDueComparator {
    bool operator()(const Bill& a, const Bill& b) const {
        if (a.paid != b.paid) return a.paid > b.paid; // Unpaid bills come first
        return a.dueDay > b.dueDay;                   // Earlier day has higher priority
    }
};

// ============================================================================
// 3. GRAPH & GREEDY ALGORITHM: MIN CASH FLOW (DEBT SIMPLIFICATION)
//    Used for: BudgetBee "Hive Split"
//    Algorithm:
//      1. Compute net balance = credit - debit for each participant.
//      2. Creditors have net > 0, Debtors have net < 0.
//      3. Using two Max-Heaps (one for creditors, one for debtors):
//         - Pick maximum debtor (owes most) and maximum creditor (is owed most).
//         - Settle min(|debt|, credit).
//         - Push back remaining balance if > 0.
//      Guarantees maximum V - 1 transactions!
//    Time Complexity: O(E + V log V) where V is participants and E is expenses.
//    Space Complexity: O(V)
// ============================================================================
class MinCashFlowSolver {
public:
    static std::vector<DebtSettlement> simplifyDebts(const std::vector<SharedExpense>& expenses) {
        // Step 1: Compute net balance for each person
        std::unordered_map<std::string, double> netBalance;

        for (const auto& exp : expenses) {
            if (exp.participants.empty() || exp.amount <= 0.001) continue;
            double share = exp.amount / exp.participants.size();

            // Payer gets credited full amount
            netBalance[exp.payer] += exp.amount;

            // Each participant owes their share
            for (const auto& person : exp.participants) {
                netBalance[person] -= share;
            }
        }

        // Priority queues for debtors and creditors
        std::priority_queue<std::pair<double, std::string>> maxCreditors;
        std::priority_queue<std::pair<double, std::string>> maxDebtors;

        for (const auto& entry : netBalance) {
            double bal = entry.second;
            if (bal > 0.01) {
                maxCreditors.push({bal, entry.first});
            } else if (bal < -0.01) {
                maxDebtors.push({-bal, entry.first}); // store positive debt
            }
        }

        std::vector<DebtSettlement> settlements;

        // Step 2: Greedily pair top debtor with top creditor
        while (!maxCreditors.empty() && !maxDebtors.empty()) {
            auto creditor = maxCreditors.top(); maxCreditors.pop();
            auto debtor = maxDebtors.top(); maxDebtors.pop();

            double settleAmount = std::min(creditor.first, debtor.first);
            settleAmount = std::round(settleAmount * 100.0) / 100.0;

            if (settleAmount > 0.009) {
                settlements.push_back({debtor.second, creditor.second, settleAmount});
            }

            double remainingCredit = creditor.first - settleAmount;
            double remainingDebit = debtor.first - settleAmount;

            if (remainingCredit > 0.01) {
                maxCreditors.push({remainingCredit, creditor.second});
            }
            if (remainingDebit > 0.01) {
                maxDebtors.push({remainingDebit, debtor.second});
            }
        }

        return settlements;
    }
};

// ============================================================================
// 4. DYNAMIC PROGRAMMING: 0/1 KNAPSACK FOR SAVINGS GOALS ALLOCATION
//    Used for: BudgetBee "Honey Optimizer"
//    Problem:
//      Given a bonus/surplus budget W, and N financial goals where:
//      - Cost to reach/fund goal = weight[i]
//      - Priority / Joy / ROI = value[i] (1 - 10 scale)
//      Find the optimal combination of goals to fully or substantially fund.
//    Time Complexity: O(N * W)
//    Space Complexity: O(W) with state tracking for recovery
// ============================================================================
struct OptimizationResult {
    double totalCost;
    int totalUtility;
    std::vector<Goal> selectedGoals;
    double remainingBudget;
};

class GoalKnapsackOptimizer {
public:
    static OptimizationResult optimizeGoals(const std::vector<Goal>& goals, double availableBudget) {
        int W = static_cast<int>(availableBudget);
        int n = goals.size();
        if (W <= 0 || n == 0) {
            return {0.0, 0, {}, availableBudget};
        }

        // DP table: dp[i][w] = max utility considering first i goals and budget w
        int step = 50; // step resolution in Rupees
        int capacity = W / step;
        if (capacity <= 0) capacity = 1;

        std::vector<std::vector<int>> dp(n + 1, std::vector<int>(capacity + 1, 0));

        std::vector<int> weights(n);
        std::vector<int> values(n);

        for (int i = 0; i < n; ++i) {
            double needed = std::max(0.0, goals[i].target - goals[i].saved);
            weights[i] = static_cast<int>(std::ceil(needed / step));
            values[i] = std::max(1, goals[i].priority);
        }

        // Standard 0/1 Knapsack recurrence
        for (int i = 1; i <= n; ++i) {
            for (int w = 0; w <= capacity; ++w) {
                if (weights[i - 1] <= w) {
                    dp[i][w] = std::max(dp[i - 1][w], dp[i - 1][w - weights[i - 1]] + values[i - 1]);
                } else {
                    dp[i][w] = dp[i - 1][w];
                }
            }
        }

        // Backtracking to find selected goals
        std::vector<Goal> chosen;
        int currW = capacity;
        double totalCost = 0.0;
        int totalUtility = dp[n][capacity];

        for (int i = n; i >= 1; --i) {
            if (dp[i][currW] != dp[i - 1][currW]) {
                chosen.push_back(goals[i - 1]);
                double needed = std::max(0.0, goals[i - 1].target - goals[i - 1].saved);
                totalCost += needed;
                currW -= weights[i - 1];
            }
        }
        std::reverse(chosen.begin(), chosen.end());

        return {totalCost, totalUtility, chosen, availableBudget - totalCost};
    }
};

// ============================================================================
// 5. STATISTICAL ANOMALY DETECTION (WELFORD'S / Z-SCORE ALGORITHM)
//    Used for: Detecting outlier spending (Bee Brain alert)
//    Complexity: O(N) Time, O(1) Auxiliary Space per category
// ============================================================================
struct Anomaly {
    Transaction tx;
    double categoryMean;
    double threshold;
};

class AnomalyDetector {
public:
    static std::vector<Anomaly> detectAnomalies(const std::vector<Transaction>& transactions, double factor = 1.5) {
        std::unordered_map<std::string, std::vector<const Transaction*>> catMap;
        for (const auto& tx : transactions) {
            if (tx.type == "out") {
                catMap[tx.category].push_back(&tx);
            }
        }

        std::vector<Anomaly> anomalies;

        for (const auto& entry : catMap) {
            const auto& list = entry.second;
            if (list.size() < 3) continue; // Need at least 3 data points

            // Calculate Mean
            double sum = 0.0;
            for (const auto* tx : list) sum += tx->amount;
            double mean = sum / list.size();

            // Calculate Standard Deviation
            double varSum = 0.0;
            for (const auto* tx : list) {
                varSum += (tx->amount - mean) * (tx->amount - mean);
            }
            double stdDev = std::sqrt(varSum / list.size());
            double threshold = mean + factor * stdDev;

            for (const auto* tx : list) {
                if (tx->amount > threshold) {
                    anomalies.push_back({*tx, mean, threshold});
                }
            }
        }

        return anomalies;
    }
};

// ============================================================================
// 6. SLIDING WINDOW & ROLLING BURN RATE ALGORITHM
//    Used for: 7-day spending velocity and day-to-day burn analytics
//    Time Complexity: O(N log N) for date sorting + O(N) sliding window
// ============================================================================
struct DailySpend {
    std::string date;
    double amount;
};

class SlidingWindowAnalytics {
public:
    static std::vector<std::pair<std::string, double>> compute7DayMovingAverage(
        const std::vector<Transaction>& transactions
    ) {
        std::map<std::string, double> dailyMap; // sorted by date
        for (const auto& tx : transactions) {
            if (tx.type == "out") {
                dailyMap[tx.date] += tx.amount;
            }
        }

        std::vector<DailySpend> days;
        for (const auto& entry : dailyMap) {
            days.push_back({entry.first, entry.second});
        }

        std::vector<std::pair<std::string, double>> movingAvg;
        if (days.empty()) return movingAvg;

        double windowSum = 0.0;
        int windowSize = 7;
        std::queue<double> q;

        for (size_t i = 0; i < days.size(); ++i) {
            windowSum += days[i].amount;
            q.push(days[i].amount);

            if ((int)q.size() > windowSize) {
                windowSum -= q.front();
                q.pop();
            }

            double avg = windowSum / q.size();
            movingAvg.push_back({days[i].date, avg});
        }

        return movingAvg;
    }
};

// ============================================================================
// 7. CENTRAL BUDGETBEE ENGINE (ORCHESTRATOR)
// ============================================================================
class BudgetBeeEngine {
private:
    std::vector<Transaction> transactions;
    std::unordered_map<std::string, double> budgets;
    std::vector<Bill> bills;
    std::vector<Goal> goals;
    std::vector<SharedExpense> sharedExpenses;
    TransactionTrie trie;
    int nextTxId;
    int nextBillId;
    int nextGoalId;

    void rebuildTrie() {
        trie.clear();
        for (const auto& tx : transactions) {
            trie.insert(tx.note, tx.id);
            trie.insert(tx.category, tx.id);
        }
    }

public:
    BudgetBeeEngine() : nextTxId(100), nextBillId(10), nextGoalId(10) {
        seedDefaults();
        rebuildTrie();
    }

    void seedDefaults() {
        transactions = {
            {1, "in", 60000, "Savings", "Salary", "2026-11-01"},
            {2, "out", 15000, "Housing", "Rent", "2026-11-01"},
            {3, "out", 2400, "Food", "Groceries", "2026-11-02"},
            {4, "out", 650, "Food", "Lunch with friends", "2026-11-03"},
            {5, "out", 540, "Food", "Food delivery", "2026-11-03"},
            {6, "out", 5200, "Food", "Party dinner", "2026-11-04"},
            {7, "out", 320, "Transport", "Cab", "2026-11-02"},
            {8, "out", 280, "Transport", "Metro card", "2026-11-03"},
            {9, "out", 1100, "Health", "Medicines", "2026-11-04"},
            {10, "out", 1800, "Personal", "Shopping", "2026-11-04"}
        };

        budgets = {
            {"Housing", 15500},
            {"Food", 7000},
            {"Utilities", 3000},
            {"Transport", 2000},
            {"Loans", 6000},
            {"Subscriptions", 1200},
            {"Health", 2000},
            {"Education", 2500},
            {"Personal", 3000},
            {"Savings", 8000}
        };

        bills = {
            {1, "Education loan EMI", "EMI", "Loans", 5400, 10, false},
            {2, "Electricity bill", "Bill", "Utilities", 1450, 15, false},
            {3, "Netflix", "Subscription", "Subscriptions", 649, 7, false},
            {4, "Wi-Fi", "Bill", "Utilities", 799, 20, false}
        };

        goals = {
            {1, "New laptop", 60000, 18000, 9},     // Target 60k, Saved 18k, Priority 9
            {2, "Goa trip", 25000, 9500, 7},        // Target 25k, Saved 9.5k, Priority 7
            {3, "Emergency Fund", 50000, 30000, 10},// Priority 10
            {4, "Noise Cancelling Headphones", 12000, 2000, 5}
        };

        sharedExpenses = {
            {"Divyanshu", 3600, "Group Dinner", {"Divyanshu", "Aman", "Rhea", "Kavya"}},
            {"Aman", 1200, "Snacks & Drinks", {"Divyanshu", "Aman", "Rhea"}},
            {"Rhea", 2000, "Cab Rides", {"Divyanshu", "Rhea", "Kavya"}},
            {"Kavya", 800, "Desserts", {"Divyanshu", "Aman", "Rhea", "Kavya"}}
        };
    }

    void addTransaction(const std::string& type, double amount, const std::string& cat, const std::string& note, const std::string& date) {
        Transaction tx{++nextTxId, type, amount, cat, note, date};
        transactions.push_back(tx);
        trie.insert(note, tx.id);
        trie.insert(cat, tx.id);
    }

    double getTotalIncome() const {
        double inc = 0;
        for (const auto& tx : transactions) if (tx.type == "in") inc += tx.amount;
        return inc;
    }

    double getTotalExpense() const {
        double exp = 0;
        for (const auto& tx : transactions) if (tx.type == "out") exp += tx.amount;
        return exp;
    }

    double getTotalBudget() const {
        double tot = 0;
        for (const auto& b : budgets) tot += b.second;
        return tot;
    }

    double getUnpaidBills() const {
        double unp = 0;
        for (const auto& b : bills) if (!b.paid) unp += b.amount;
        return unp;
    }

    double getSafeToSpend() const {
        return getTotalBudget() - getTotalExpense() - getUnpaidBills();
    }

    // DSA Queries
    std::vector<Transaction> searchTrie(const std::string& prefix) const {
        std::vector<int> ids = trie.searchPrefix(prefix);
        std::vector<Transaction> res;
        for (int id : ids) {
            for (const auto& tx : transactions) {
                if (tx.id == id) {
                    res.push_back(tx);
                    break;
                }
            }
        }
        return res;
    }

    std::vector<Transaction> getTopExpenses(int k) const {
        return TopKExpenseHeap::getTopK(transactions, k);
    }

    std::vector<Anomaly> getAnomalies() const {
        return AnomalyDetector::detectAnomalies(transactions, 1.5);
    }

    std::vector<DebtSettlement> simplifyGroupDebts() const {
        return MinCashFlowSolver::simplifyDebts(sharedExpenses);
    }

    OptimizationResult optimizeGoalsWithSurplus(double surplus) const {
        return GoalKnapsackOptimizer::optimizeGoals(goals, surplus);
    }

    std::vector<std::pair<std::string, double>> getMovingAverage() const {
        return SlidingWindowAnalytics::compute7DayMovingAverage(transactions);
    }

    // JSON Serializers for HTTP API
    std::string toJSONSummary() const {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(2);
        ss << "{\n";
        ss << "  \"income\": " << getTotalIncome() << ",\n";
        ss << "  \"expense\": " << getTotalExpense() << ",\n";
        ss << "  \"totalBudget\": " << getTotalBudget() << ",\n";
        ss << "  \"unpaidBills\": " << getUnpaidBills() << ",\n";
        ss << "  \"safeToSpend\": " << getSafeToSpend() << ",\n";
        ss << "  \"transactionCount\": " << transactions.size() << ",\n";
        ss << "  \"goalCount\": " << goals.size() << ",\n";
        ss << "  \"billCount\": " << bills.size() << "\n";
        ss << "}";
        return ss.str();
    }

    std::string toJSONTopK(int k) const {
        auto topK = getTopExpenses(k);
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(2);
        ss << "[\n";
        for (size_t i = 0; i < topK.size(); ++i) {
            const auto& tx = topK[i];
            ss << "  {\"id\": " << tx.id << ", \"amount\": " << tx.amount 
               << ", \"category\": \"" << tx.category << "\", \"note\": \"" << tx.note 
               << "\", \"date\": \"" << tx.date << "\"}";
            if (i + 1 < topK.size()) ss << ",";
            ss << "\n";
        }
        ss << "]";
        return ss.str();
    }

    std::string toJSONSplit() const {
        auto settlements = simplifyGroupDebts();
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(2);
        ss << "{\n";
        ss << "  \"algorithm\": \"Min Cash Flow (Graph + Max-Heap Greedy)\",\n";
        ss << "  \"settlements\": [\n";
        for (size_t i = 0; i < settlements.size(); ++i) {
            const auto& s = settlements[i];
            ss << "    {\"debtor\": \"" << s.debtor << "\", \"creditor\": \"" << s.creditor 
               << "\", \"amount\": " << s.amount << "}";
            if (i + 1 < settlements.size()) ss << ",";
            ss << "\n";
        }
        ss << "  ]\n}";
        return ss.str();
    }

    std::string toJSONOptimize(double surplus) const {
        auto opt = optimizeGoalsWithSurplus(surplus);
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(2);
        ss << "{\n";
        ss << "  \"algorithm\": \"0/1 Knapsack Dynamic Programming\",\n";
        ss << "  \"surplus\": " << surplus << ",\n";
        ss << "  \"totalCost\": " << opt.totalCost << ",\n";
        ss << "  \"remainingBudget\": " << opt.remainingBudget << ",\n";
        ss << "  \"totalUtilityScore\": " << opt.totalUtility << ",\n";
        ss << "  \"selectedGoals\": [\n";
        for (size_t i = 0; i < opt.selectedGoals.size(); ++i) {
            const auto& g = opt.selectedGoals[i];
            ss << "    {\"id\": " << g.id << ", \"name\": \"" << g.name 
               << "\", \"target\": " << g.target << ", \"saved\": " << g.saved 
               << ", \"priority\": " << g.priority << "}";
            if (i + 1 < opt.selectedGoals.size()) ss << ",";
            ss << "\n";
        }
        ss << "  ]\n}";
        return ss.str();
    }

    std::string toJSONAnomalies() const {
        auto anoms = getAnomalies();
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(2);
        ss << "[\n";
        for (size_t i = 0; i < anoms.size(); ++i) {
            const auto& a = anoms[i];
            ss << "  {\"id\": " << a.tx.id << ", \"note\": \"" << a.tx.note 
               << "\", \"amount\": " << a.tx.amount << ", \"category\": \"" << a.tx.category 
               << "\", \"categoryMean\": " << a.categoryMean << ", \"threshold\": " << a.threshold << "}";
            if (i + 1 < anoms.size()) ss << ",";
            ss << "\n";
        }
        ss << "]";
        return ss.str();
    }

    // Interactive CLI Interface
    void runCLI() {
        std::cout << "\n=======================================================\n";
        std::cout << "  🐝 BudgetBee C++ Core Backend Engine (DSA Powered)    \n";
        std::cout << "=======================================================\n";

        while (true) {
            std::cout << "\n[MENU] Choose a DSA operation:\n";
            std::cout << "  1. View Financial Summary & Safe-to-Spend\n";
            std::cout << "  2. Search Transactions by Prefix (Trie - O(L))\n";
            std::cout << "  3. Extract Top-K High Expenses (Min-Heap - O(N log K))\n";
            std::cout << "  4. Settle Group Expenses (Min Cash Flow Graph)\n";
            std::cout << "  5. Optimize Savings Goals (0/1 Knapsack DP)\n";
            std::cout << "  6. Detect Spending Anomalies (Z-Score / StdDev)\n";
            std::cout << "  7. 7-Day Moving Average (Sliding Window)\n";
            std::cout << "  8. Add a New Transaction\n";
            std::cout << "  9. Start Local REST HTTP API Server (Port 8080)\n";
            std::cout << "  0. Exit\n";
            std::cout << "Enter choice: ";

            int choice;
            if (!(std::cin >> choice)) {
                std::cin.clear();
                std::string ignore;
                std::getline(std::cin, ignore);
                continue;
            }

            if (choice == 0) {
                std::cout << "Exiting BudgetBee C++ Backend. Goodbye! 🐝\n";
                break;
            } else if (choice == 1) {
                std::cout << "\n--- Financial Summary ---\n";
                std::cout << "Total Income:      ₹" << std::fixed << std::setprecision(2) << getTotalIncome() << "\n";
                std::cout << "Total Expenses:    ₹" << getTotalExpense() << "\n";
                std::cout << "Total Budget:      ₹" << getTotalBudget() << "\n";
                std::cout << "Unpaid Bills:      ₹" << getUnpaidBills() << "\n";
                std::cout << "Safe to Spend:     ₹" << getSafeToSpend() << "\n";
            } else if (choice == 2) {
                std::cout << "Enter prefix to search (note or category): ";
                std::string prefix;
                std::cin >> prefix;
                auto results = searchTrie(prefix);
                std::cout << "\n[Trie Results for '" << prefix << "']: " << results.size() << " match(es) found:\n";
                for (const auto& tx : results) {
                    std::cout << "  • ID " << tx.id << " | ₹" << tx.amount << " | " << tx.category << " | \"" << tx.note << "\" (" << tx.date << ")\n";
                }
            } else if (choice == 3) {
                std::cout << "Enter K (number of top expenses to extract): ";
                int k;
                std::cin >> k;
                auto topK = getTopExpenses(k);
                std::cout << "\n[Min-Heap O(N log K) Top-" << k << " Expenses]:\n";
                for (size_t i = 0; i < topK.size(); ++i) {
                    std::cout << "  #" << (i + 1) << " ₹" << topK[i].amount << " - " << topK[i].note << " [" << topK[i].category << "]\n";
                }
            } else if (choice == 4) {
                std::cout << "\n[Hive Split: Min Cash Flow Debt Simplification]\n";
                std::cout << "Original Shared Expenses Count: " << sharedExpenses.size() << "\n";
                auto settlements = simplifyGroupDebts();
                std::cout << "Optimal Minimal Transactions to Settle All Debts (" << settlements.size() << " transactions):\n";
                for (const auto& s : settlements) {
                    std::cout << "  💸 " << s.debtor << " pays " << s.creditor << " ₹" << std::fixed << std::setprecision(2) << s.amount << "\n";
                }
            } else if (choice == 5) {
                std::cout << "Enter your extra surplus/bonus amount (₹): ";
                double surplus;
                std::cin >> surplus;
                auto opt = optimizeGoalsWithSurplus(surplus);
                std::cout << "\n[0/1 Knapsack DP Goal Optimization for ₹" << surplus << "]:\n";
                std::cout << "Total Utility Score Achieved: " << opt.totalUtility << "\n";
                std::cout << "Total Cost Utilized:          ₹" << opt.totalCost << "\n";
                std::cout << "Surplus Unused:               ₹" << opt.remainingBudget << "\n";
                std::cout << "Recommended Goals to Fund Now:\n";
                for (const auto& g : opt.selectedGoals) {
                    std::cout << "  🎯 " << g.name << " (Priority: " << g.priority << "/10, Target: ₹" << g.target << ", Needed: ₹" << (g.target - g.saved) << ")\n";
                }
            } else if (choice == 6) {
                std::cout << "\n[Statistical Anomaly Detection (Welford's > Mean + 1.5*SD)]:\n";
                auto anoms = getAnomalies();
                if (anoms.empty()) {
                    std::cout << "No anomalous expenses detected! Spending is well within limits.\n";
                } else {
                    for (const auto& a : anoms) {
                        std::cout << "  ⚠️ High Spend: ₹" << a.tx.amount << " on '" << a.tx.note 
                                  << "' in " << a.tx.category << " (Category Mean: ₹" << a.categoryMean 
                                  << ", Threshold: ₹" << a.threshold << ")\n";
                    }
                }
            } else if (choice == 7) {
                std::cout << "\n[Sliding Window 7-Day Moving Average]:\n";
                auto mavg = getMovingAverage();
                for (const auto& p : mavg) {
                    std::cout << "  Date: " << p.first << " | 7-Day Rolling Avg: ₹" << std::fixed << std::setprecision(2) << p.second << "\n";
                }
            } else if (choice == 8) {
                std::string type, cat, note, date;
                double amt;
                std::cout << "Type (in/out): "; std::cin >> type;
                std::cout << "Amount (₹): "; std::cin >> amt;
                std::cout << "Category: "; std::cin >> cat;
                std::cout << "Note: "; 
                std::cin.ignore();
                std::getline(std::cin, note);
                std::cout << "Date (YYYY-MM-DD): "; std::cin >> date;
                addTransaction(type, amt, cat, note, date);
                std::cout << "✓ Transaction recorded and indexed into Trie!\n";
            } else if (choice == 9) {
                startHTTPServer(8080);
            }
        }
    }

    // Built-in Lightweight Zero-Dependency HTTP Server
    void startHTTPServer(int port) {
#if defined(_WIN32) || defined(_WIN64)
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            std::cerr << "Failed to initialize Winsock.\n";
            return;
        }
#endif
        socket_t server_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd < 0) {
            std::cerr << "Error: Could not create socket.\n";
            return;
        }

        int opt = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(port);

        if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
            std::cerr << "Error: Bind failed on port " << port << " (Port may be in use).\n";
            CLOSE_SOCKET(server_fd);
            return;
        }

        if (listen(server_fd, 10) < 0) {
            std::cerr << "Error: Listen failed.\n";
            CLOSE_SOCKET(server_fd);
            return;
        }

        std::cout << "\n=======================================================\n";
        std::cout << "🚀 BudgetBee C++ REST Server is RUNNING on http://localhost:" << port << "\n";
        std::cout << "   Endpoints available:\n";
        std::cout << "   - GET  /api/summary\n";
        std::cout << "   - GET  /api/topk?k=5\n";
        std::cout << "   - GET  /api/split\n";
        std::cout << "   - GET  /api/optimize?surplus=15000\n";
        std::cout << "   - GET  /api/anomalies\n";
        std::cout << "   - GET  /api/search?q=<prefix>\n";
        std::cout << "   Press Ctrl+C in terminal to stop server.\n";
        std::cout << "=======================================================\n\n";

        while (true) {
            sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);
            socket_t client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
            if (client_fd < 0) {
                break;
            }

            char buffer[4096];
            memset(buffer, 0, sizeof(buffer));
            int bytesRead = recv(client_fd, buffer, sizeof(buffer) - 1, 0);

            if (bytesRead > 0) {
                std::string request(buffer);
                std::istringstream reqStream(request);
                std::string method, path, protocol;
                reqStream >> method >> path >> protocol;

                // Handle CORS preflight
                if (method == "OPTIONS") {
                    std::string res = 
                        "HTTP/1.1 204 No Content\r\n"
                        "Access-Control-Allow-Origin: *\r\n"
                        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                        "Access-Control-Allow-Headers: Content-Type\r\n"
                        "Content-Length: 0\r\n\r\n";
                    send(client_fd, res.c_str(), res.length(), 0);
                    CLOSE_SOCKET(client_fd);
                    continue;
                }

                std::string responseBody = "{}";
                int statusCode = 200;
                std::string statusText = "OK";

                if (path == "/api/summary") {
                    responseBody = toJSONSummary();
                } else if (path.rfind("/api/topk", 0) == 0) {
                    int k = 5;
                    size_t kpos = path.find("k=");
                    if (kpos != std::string::npos) {
                        k = std::stoi(path.substr(kpos + 2));
                    }
                    responseBody = toJSONTopK(k);
                } else if (path == "/api/split") {
                    responseBody = toJSONSplit();
                } else if (path.rfind("/api/optimize", 0) == 0) {
                    double surplus = 15000;
                    size_t spos = path.find("surplus=");
                    if (spos != std::string::npos) {
                        surplus = std::stod(path.substr(spos + 8));
                    }
                    responseBody = toJSONOptimize(surplus);
                } else if (path == "/api/anomalies") {
                    responseBody = toJSONAnomalies();
                } else if (path.rfind("/api/search", 0) == 0) {
                    std::string query = "";
                    size_t qpos = path.find("q=");
                    if (qpos != std::string::npos) {
                        query = path.substr(qpos + 2);
                    }
                    auto list = searchTrie(query);
                    std::ostringstream ss;
                    ss << "[";
                    for (size_t i = 0; i < list.size(); ++i) {
                        ss << "{\"id\":" << list[i].id << ",\"note\":\"" << list[i].note 
                           << "\",\"cat\":\"" << list[i].category << "\",\"amt\":" << list[i].amount << "}";
                        if (i + 1 < list.size()) ss << ",";
                    }
                    ss << "]";
                    responseBody = ss.str();
                } else {
                    statusCode = 404;
                    statusText = "Not Found";
                    responseBody = "{\"error\": \"Endpoint not found\"}";
                }

                std::ostringstream httpRes;
                httpRes << "HTTP/1.1 " << statusCode << " " << statusText << "\r\n";
                httpRes << "Content-Type: application/json; charset=utf-8\r\n";
                httpRes << "Access-Control-Allow-Origin: *\r\n";
                httpRes << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
                httpRes << "Access-Control-Allow-Headers: Content-Type\r\n";
                httpRes << "Content-Length: " << responseBody.length() << "\r\n";
                httpRes << "Connection: close\r\n\r\n";
                httpRes << responseBody;

                std::string fullResponse = httpRes.str();
                send(client_fd, fullResponse.c_str(), fullResponse.length(), 0);
            }
            CLOSE_SOCKET(client_fd);
        }

        CLOSE_SOCKET(server_fd);
    }
};

// ============================================================================
// MAIN ENTRY POINT
// ============================================================================
int main(int argc, char* argv[]) {
    BudgetBeeEngine engine;

    if (argc > 1) {
        std::string arg = argv[1];
        if (arg == "--server" || arg == "-s") {
            int port = 8080;
            if (argc > 2) port = std::stoi(argv[2]);
            engine.startHTTPServer(port);
            return 0;
        } else if (arg == "--summary") {
            std::cout << engine.toJSONSummary() << "\n";
            return 0;
        } else if (arg == "--split") {
            std::cout << engine.toJSONSplit() << "\n";
            return 0;
        } else if (arg == "--optimize") {
            double surplus = (argc > 2) ? std::stod(argv[2]) : 15000.0;
            std::cout << engine.toJSONOptimize(surplus) << "\n";
            return 0;
        } else if (arg == "--topk") {
            int k = (argc > 2) ? std::stoi(argv[2]) : 5;
            std::cout << engine.toJSONTopK(k) << "\n";
            return 0;
        }
    }

    // Default to interactive CLI
    engine.runCLI();
    return 0;
}