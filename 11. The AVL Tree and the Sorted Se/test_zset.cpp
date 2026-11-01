// learning/ch11/test_zset.cpp      (NOT from the book: an extra safety net)
//
// The chapter leaves most of the sorted-set code to the reader, so
// this test checks it hard. It runs thousands of random zadd / zrem /
// zscore / zquery operations on our ZSet and on a trusted reference
// built from the standard library (std::map + std::set). After the
// operations it checks that both agree: same contents, same order,
// same query answers.
//
// Build: g++ -std=c++17 -Wall -Wextra -O2 -o test_zset test_zset.cpp zset.cpp avl.cpp hashtable.cpp
// Run:   ./test_zset

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "common.h"
#include "zset.h"

using namespace std;

typedef pair<double, string> Item;   // (score, name): the same ordering rule as zless

// Read the whole ZSet in sorted order, by starting at the smallest
// item and stepping forward with avl_offset(+1).
static vector<Item> dump(ZSet *z) {
    vector<Item> out;
    ZNode *n = zset_query(z, -INFINITY, "", 0, 0);
    while (n) {
        out.push_back(Item(n->score, string(n->name, n->len)));
        AVLNode *next = avl_offset(&n->tree, 1);
        n = next ? container_of(next, ZNode, tree) : nullptr;
    }
    return out;
}

// Check that our ZSet and the reference hold exactly the same data.
static void verify(ZSet *z, const map<string, double> &m, const set<Item> &s) {
    vector<Item> got = dump(z);
    vector<Item> want(s.begin(), s.end());
    assert(got == want);                                  // same items in the same order
    assert(avl_cnt(z->tree) == s.size());                 // the tree's size counter is right
    assert(hm_size(&z->hmap) == s.size());                // the hashtable holds the same number
    for (const auto &kv : m) {                            // every name is findable with the right score
        ZNode *n = zset_lookup(z, kv.first.data(), kv.first.size());
        assert(n && n->score == kv.second);
    }
}

static void run(const vector<string> &names, const vector<double> &scores, int ops) {
    ZSet z;
    map<string, double> m;      // reference: name -> score
    set<Item> s;                // reference: sorted (score, name)

    for (int it = 0; it < ops; it++) {
        const string &name = names[rand() % names.size()];
        int op = rand() % 4;

        if (op <= 1) {
            // ---- zadd (add or update) ----
            double score = scores[rand() % scores.size()];
            bool added = zset_add(&z, name.data(), name.size(), score);
            auto f = m.find(name);
            assert(added == (f == m.end()));
            if (f != m.end()) {
                s.erase(Item(f->second, name));
            }
            m[name] = score;
            s.insert(Item(score, name));
        } else if (op == 2) {
            // ---- zrem ----
            ZNode *n = zset_pop(&z, name.data(), name.size());
            auto f = m.find(name);
            assert((n != nullptr) == (f != m.end()));
            if (n) {
                znode_del(n);
                s.erase(Item(f->second, name));
                m.erase(f);
            }
        } else {
            // ---- zscore ----
            ZNode *n = zset_lookup(&z, name.data(), name.size());
            auto f = m.find(name);
            assert((n != nullptr) == (f != m.end()));
            if (n) {
                assert(n->score == f->second);
            }

            // ---- zquery with a random starting tuple and offset ----
            double qscore = (rand() % 3 == 0) ? (double)(rand() % 20 - 5) : scores[rand() % scores.size()];
            const string &qname = (rand() % 2) ? names[rand() % names.size()] : string();
            int64_t offset = rand() % 21 - 10;

            ZNode *got = zset_query(&z, qscore, qname.data(), qname.size(), offset);

            // expected answer from the reference
            auto lb = s.lower_bound(Item(qscore, qname));
            if (lb == s.end()) {
                assert(got == nullptr);                      // no start item at all
            } else {
                int64_t idx = (int64_t)distance(s.begin(), lb) + offset;
                if (idx < 0 || idx >= (int64_t)s.size()) {
                    assert(got == nullptr);                  // offset ran off an end
                } else {
                    auto want = s.begin();
                    advance(want, idx);
                    assert(got != nullptr);
                    assert(got->score == want->first);
                    assert(string(got->name, got->len) == want->second);
                }
            }
        }

        if (it < 1500 || it % 500 == 0) {
            verify(&z, m, s);
        }
    }
    verify(&z, m, s);
    printf("  %d random operations OK (final size %zu)\n", ops, s.size());
    zset_dispose(&z);
}

int main() {
    // Phase 1: few names (all strings of 'a'/'b' up to length 3, including the
    // empty name) and few scores: lots of ties and prefix-name edge cases.
    vector<string> small_names = {"", "a", "b", "aa", "ab", "ba", "bb",
                                  "aaa", "aab", "aba", "abb", "baa", "bab", "bba", "bbb"};
    vector<double> small_scores = {-1, 0, 1, 1, 2.5, 7};
    printf("phase 1: many ties, tiny set\n");
    run(small_names, small_scores, 30000);

    // Phase 2: thousands of names so the tree gets deep and the hashtable resizes.
    vector<string> big_names;
    for (int i = 0; i < 4000; i++) {
        big_names.push_back("player" + to_string(i));
    }
    vector<double> big_scores;
    for (int i = 0; i < 300; i++) {
        big_scores.push_back(i * 0.5);
    }
    printf("phase 2: thousands of names (tree and hashtable grow)\n");
    run(big_names, big_scores, 60000);

    printf("ALL TESTS PASSED\n");
    return 0;
}
