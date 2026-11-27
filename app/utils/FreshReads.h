#pragma once

#include <QList>

#include <functional>
#include <utility>

// Reads of the installed packages, newest first, and what waits for one
// started after it asked. A read that fails is no answer: what waits learns
// it got none, and plans nothing on what was known before. Header-only, so
// it is under test.
namespace logos {

class FreshReads {
public:
    using Then = std::function<void(bool read)>;

    // A read starts; its generation. One started since supersedes it.
    int begin() { return ++m_generation; }
    bool isNewest(int generation) const { return generation == m_generation; }

    // `then` runs once a read started after now is in, or failed. `start`
    // starts it; false when none can start, which is a failed read too.
    void afterNext(const std::function<bool()>& start, Then then)
    {
        const int wanted = m_generation + 1;
        if (!start()) {
            then(false);
            return;
        }
        m_waiting.append({wanted, std::move(then)});
    }

    // The newest read, `generation`, is in (`read`) or failed.
    void done(int generation, bool read)
    {
        if (!isNewest(generation)) return;
        const QList<Waiter> waiting = std::exchange(m_waiting, {});
        for (const Waiter& w : waiting) {
            if (w.generation <= generation) w.then(read);
            else m_waiting.append(w);
        }
    }

private:
    struct Waiter {
        int generation = 0;
        Then then;
    };
    int m_generation = 0;
    QList<Waiter> m_waiting;
};

}  // namespace logos
