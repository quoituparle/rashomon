#include <atomic>
template <unsigned CAP> struct Queue {
	struct Cell { std::atomic<unsigned> seq; int data; };
	Cell cells[CAP];
	bool push(int v) { Cell &c = cells[0]; c.data = v; c.seq = 1; return true; }
	bool pop(int &o) { Cell &c = cells[0]; if (c.seq.load() == 0) return false; o = c.data; return true; }
};
