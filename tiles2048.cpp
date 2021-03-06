#include "glfontstash.h"
#include "tinythread.h"
#include "mintomic/mintomic.h"

#define CRAZY_VERBOSE_CACHE_DEBUGGER 0
#define USE_CACHE_VERIFICATION_MAP 0
#define PRINT_BOARD_STATE 0
#define PRINT_CACHE_STATS 0

#include <GLFW/glfw3.h>

#if USE_CACHE_VERIFICATION_MAP
#include <map>
#endif

#include <cstring>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <climits>
#include <cassert>
#include <stdint.h>

template <typename T>
static T min(T a, T b) { return (a < b ? a : b); }

template <typename T>
static T max(T a, T b) { return (a > b ? a : b); }

template <typename T>
static T clamp(T x, T a, T b) { return (x < a ? a : (x > b ? b : x)); }

template <typename T>
static T signum(T a) { return T(T(0) < a) - T(a < T(0)); }

enum BoardConfig {
	TILES_X = 4,
	TILES_Y = 4,
	NUM_TILES = TILES_X * TILES_Y,
	MAX_POWER = 15
};

// note: if you change this you must change DIR_DX and DIR_DY
enum MoveDir {
	MOVE_LEFT,
	MOVE_RIGHT,
	MOVE_UP,
	MOVE_DOWN
};
// note: depends on order of values in enum MoveDir
static const int DIR_DX[4] = { -1, 1, 0, 0 };
static const int DIR_DY[4] = { 0, 0, -1, 1 };

struct RNG {
	uint32_t x, y, z, w;

	void reset(uint32_t seed = 0u) {
		x = seed ? seed : 123456789u;
		y = x^(x<<13); y ^= (y >> 17); y ^= (y << 5);
		z = y^(y<<13); z ^= (z >> 17); z ^= (z << 5);
		w = z^(z<<13); w ^= (w >> 17); w ^= (w << 5);
	}

	uint32_t next32() {
		uint32_t t = x^(x<<15); t = (w^(w>>21)) ^ (t^(t>>4));
		x = y; y = z; z = w; w = t;
		return t;
	}

	uint64_t next64() {
		const uint64_t a = next32();
		const uint64_t b = next32();
		return (a << 32) | b;
	}

	int next_n(int n) {
		// see: http://www.azillionmonkeys.com/qed/random.html
		assert(n > 0);
		const uint32_t range = UINT32_MAX - (UINT32_MAX % n);
		uint32_t value;
		do { value = next32(); } while (value >= range);
		value = (value / ((range - 1) / (uint32_t)n + 1));
		assert(value < (uint32_t)n);
		return value;
	}
};

typedef uint8_t BoardState[NUM_TILES];

#define PRINT_ANIM 0

struct AnimCurve {
	enum { MAX_KEYS = 8 };
	float ky[MAX_KEYS];
	float kt[MAX_KEYS];
	int nkeys;

	void reset() {
		nkeys = 0;
	}

	void push(float t, float y) {
		assert(nkeys < MAX_KEYS);
		assert(t >= 0.0f);
		assert(nkeys == 0 || t > kt[nkeys-1]);
		ky[nkeys] = y;
		kt[nkeys] = t;
		++nkeys;
	}

	float eval(float at) const {
		if (nkeys == 0) { return 0.0f; }
		if (nkeys == 1) { return ky[0]; }

		at = max(at, 0.0f);
		for (int i = 1; i < nkeys; ++i) {
			if (at < kt[i]) {
				float alpha = (at - kt[i-1]) / (kt[i] - kt[i-1]);
				return (1.0f - alpha)*ky[i-1] + alpha*ky[i];
			}
		}
		return ky[nkeys - 1];
	}

	float period() const {
		return (nkeys == 0 ? 0.0f : kt[nkeys-1]);
	}
};

static void tile_idx_to_xy(int where, float *x, float *y) {
	assert(where >= 0 && where < NUM_TILES);
	assert(x);
	assert(y);
	*x = 128.0f * (where % TILES_X);
	*y = 128.0f * (where / TILES_X);
}

struct TileAnim {
	int value;
	AnimCurve x;
	AnimCurve y;
	AnimCurve scale;
	void reset() {
		value = 15;
		x.reset();
		y.reset();
		scale.reset();
	}
	float period() const {
		return max(max(x.period(), y.period()), scale.period());
	}
};

struct ScoreAnim {
	int score;
	AnimCurve x;
	AnimCurve y;
	AnimCurve alpha;
	void reset() {
		score = 0;
		x.reset();
		y.reset();
		alpha.reset();
	}
	float period() const {
		return max(max(x.period(), y.period()), alpha.period());
	}
};

struct Board;

struct AnimState {
	TileAnim tiles[NUM_TILES*2];
	ScoreAnim scores[NUM_TILES];
	float period;
	int ntiles;
	int nscores;
	bool moved;

	bool tiles_changed() const {
		return moved;
	}

	void reset() {
		period = 0.0f;
		ntiles = 0;
		nscores = 0;
		moved = false;
	}

	void add_slide(int from, int to, int value) {
		assert(ntiles < NUM_TILES*2);
		assert(to >= 0 && to < NUM_TILES);
		assert(from >= 0 && from < NUM_TILES);

		float x0, y0, x1, y1;
		tile_idx_to_xy(from, &x0, &y0);
		tile_idx_to_xy(to, &x1, &y1);

		TileAnim &tile = tiles[ntiles++];
		tile.reset();
		tile.value = value;
		tile.x.push(0, x0);
		tile.x.push(100, x1);
		tile.y.push(0, y0);
		tile.y.push(100, y1);
		tile.scale.push(0, 1.0f);

		period = max(period, tile.period());
	}

	void add_slide_and_vanish(int from, int to, int value) {
		add_slide(from, to, value);
		tiles[ntiles-1].scale.push( 80, 1.0f);
		tiles[ntiles-1].scale.push(200, 0.2f);

		period = max(period, tiles[ntiles-1].period());
	}

	void add_pop_tile(int where, int value) {
		assert(ntiles < NUM_TILES*2);
		assert(where >= 0 && where < NUM_TILES);

		float x, y;
		tile_idx_to_xy(where, &x, &y);
		TileAnim &tile = tiles[ntiles++];
		tile.reset();
		tile.value = value;
		tile.x.push(0, x);
		tile.y.push(0, y);
		tile.scale.push(0, 0.0f);
		tile.scale.push(89.9999f, 0.0f);
		tile.scale.push(90, 0.2f);
		tile.scale.push(120, 1.25f);
		tile.scale.push(200, 1.0f);

		period = max(period, tile.period());
	}

	void add_place_tile(int where, int value) {
		assert(ntiles < NUM_TILES*2);
		assert(where >= 0 && where < NUM_TILES);

		float x, y;
		tile_idx_to_xy(where, &x, &y);
		TileAnim &tile = tiles[ntiles++];
		tile.reset();
		tile.value = value;
		tile.x.push(0.0f, x);
		tile.y.push(0.0f, y);
		tile.scale.push(0, 0.0f);
		tile.scale.push(89.9999f, 0.0f);
		tile.scale.push(90, 0.2f);
		tile.scale.push(200, 1.0f);

		period = max(period, tile.period());
	}

	void add_score_slide(int where, int value) {
		assert(nscores < NUM_TILES);
		float x, y;
		tile_idx_to_xy(where, &x, &y);
		ScoreAnim &score = scores[nscores++];
		score.reset();
		score.score = 1 << value;
		score.x.push(0, x);
		score.y.push(0, y);
		score.y.push(100, y);
		score.y.push(500, y-96.0f);
		score.alpha.push(0, 0.0f);
		score.alpha.push(90, 0.0f);
		score.alpha.push(100, 0.4f);
		score.alpha.push(500, 1.0f);

		period = max(period, score.period());
	}

	void merge(int from0, int from1, int to, int old_value) {
		add_slide_and_vanish(from0, to, old_value);
		add_slide_and_vanish(from1, to, old_value);
		add_pop_tile(to, old_value + 1);
		add_score_slide(to, old_value + 1);
		moved = true;
	}

	void slide(int from, int to, int value) {
		add_slide(from, to, value);
		if (from != to) { moved = true; }
	}

	void blank(int /*where*/) {}

	void new_tile(int where, int value) {
		add_place_tile(where, value);
		moved = true;
	}
};

struct Board {
	BoardState state;

	void reset() {
		memset(&state, 0, sizeof(state));
	}

	int count_free(uint8_t *free = 0) const {
		int nfree = 0;
		for (int i = 0; i < NUM_TILES; ++i) {
			if (free && (state[i] == 0)) { free[nfree] = i; }
			nfree += (state[i] == 0);
		}
		assert(nfree >= 0 && nfree <= NUM_TILES);
		return nfree;
	}

	bool has_direct_matches() const {
		/* check rows */
		for (int i = 0; i < TILES_Y; ++i) {
			const uint8_t *at = (state + i*TILES_X);
			for (int j = 1; j < TILES_X; ++j) {
				if (at[0] && (at[0] == at[1])) { return true; }
				++at;
			}
		}

		/* check columns */
		for (int j = 0; j < TILES_X; ++j) {
			const uint8_t *at = (state + j);
			for (int i = 1; i < TILES_Y; ++i) {
				if (at[0] && (at[0] == at[TILES_X])) { return true; }
				at += TILES_X;
			}
		}
		return false;
	}

	bool finished() const {
		return (count_free() == 0 && !has_direct_matches());
	}

	void place(int count, AnimState *anim, RNG &rng) {
		assert(count > 0);
		uint8_t free[NUM_TILES];
		int nfree = count_free(free);
		while (count && nfree) {
			int value = (rng.next_n(10) < 9 ? 1 : 2);
			int which = rng.next_n(nfree);
			assert(which >= 0 && which < nfree);

			state[free[which]] = value;
			if (anim) { anim->new_tile(free[which], value); }

			// could do this by swapping the last value into free[which],
			// but that changes the order of slots which means that
			// place(1); place(1); would behave differently to place(2);
			for (int i = which + 1; i < nfree; ++i) {
				assert(i < NUM_TILES);
				free[i-1] = free[i];
			}
			--nfree;
			--count;
		}
	}

	bool tilt(int dx, int dy, AnimState *anim = 0, int *score = 0) {
		assert((dx && !dy) || (dy && !dx));

		int begin = ((dx | dy) > 0 ? NUM_TILES - 1 : 0);
		int step_major = -(dx*TILES_X + dy);
		int step_minor = -(dy*TILES_X + dx);
		int n = (dx ? TILES_Y : TILES_X);
		int m = (dx ? TILES_X : TILES_Y);

		bool moved = false;

		for (int i = 0; i < n; ++i) {
			int stop = begin + m*step_minor;
			int from = begin, to = begin;

			int last_value = 0;
			int last_from = from;
			while (from != stop) {
				if (state[from]) {
					if (last_value) {
						if (last_value == state[from]) {
							if (anim) { anim->merge(last_from, from, to, last_value); }
							if (score) { *score += (1 << (last_value + 1)); }
							moved = true;
							state[to] = last_value + 1;
							last_value = 0;
						} else {
							if (anim) { anim->slide(last_from, to, last_value); }
							if (last_from != to) { moved = true; }
							int tmp = state[from];
							state[to] = last_value;
							last_value = tmp;
							last_from = from;
						}
						to += step_minor;
					} else {
						last_value = state[from];
						last_from = from;
					}
				}
				from += step_minor;
			}
			if (last_value) {
				if (anim) { anim->slide(last_from, to, last_value); }
				if (last_from != to) { moved = true; }
				state[to] = last_value;
				to += step_minor;
			}
			while (to != stop) {
				if (anim) { anim->blank(to); }
				state[to] = 0;
				to += step_minor;
			}

			begin += step_major;
		}

		return moved;
	}

	bool move(int dir, AnimState *anim, RNG &rng, int *score) {
		assert(dir >= 0 && dir < 4);
		if (anim) { anim->reset(); }
		bool moved = tilt(DIR_DX[dir], DIR_DY[dir], anim, score);
		if (moved) { place(1, anim, rng); }
		return moved;
	}
};

struct BoardHistory {
	struct HistoryState {
		Board board;
		RNG rng;
		int score;
		void reset() {
			board.reset();
			rng.reset();
			score = 0;
		}
		void reset(const RNG &initial_rng) {
			board.reset();
			rng = initial_rng;
			score = 0;
		}
		void reset(const Board &initial_board, const RNG &initial_rng) {
			board = initial_board;
			rng = initial_rng;
			score = 0;
		}
		void reset(uint32_t seed) {
			board.reset();
			rng.reset(seed);
			score = 0;
		}
		void place(int n, AnimState *anim) {
			board.place(n, anim, rng);
		}
		bool move(int dir, AnimState *anim) {
			return board.move(dir, anim, rng, &score);
		}
	};

	enum { MAX_UNDO = 4096 };
	HistoryState history[MAX_UNDO];
	int current;
	int undo_avail;
	int redo_avail;

	void clear_history() {
		// retain RNG state
		RNG rng = get_rng();
		history[0].reset(rng);
		current = 0;
		undo_avail = 0;
		redo_avail = 0;
	}

	void reset(uint32_t seed = 0u) {
		history[0].reset(seed);
		current = 0;
		undo_avail = 0;
		redo_avail = 0;
	}

	void reset(const Board &board, const RNG &initial_state) {
		history[0].reset(board, initial_state);
		current = 0;
		undo_avail = 0;
		redo_avail = 0;
	}

	void new_game(AnimState &anim) {
		clear_history();
		history[0].place(2, &anim);
	}

	const Board &get() const { return history[current].board; }
	const RNG &get_rng() const { return history[current].rng; }
	int get_score() const { return history[current].score; }

	Board &undo() {
		if (undo_avail) {
			--undo_avail;
			++redo_avail;
			current = (current + MAX_UNDO - 1) % MAX_UNDO;
		}
		return history[current].board;
	}

	Board &redo() {
		if (redo_avail) {
			--redo_avail;
			++undo_avail;
			current = (current + 1) % MAX_UNDO;
		}
		return history[current].board;
	}

	void move(int dir, AnimState &anim) {
		HistoryState next = history[current];
		bool moved = next.move(dir, &anim);

		if (moved) {
			current = (current + 1) % MAX_UNDO;
			history[current] = next;
			if (undo_avail < MAX_UNDO) { ++undo_avail; }
			redo_avail = 0;
		}
	}
};

static uint64_t pack_board_state(const Board &board) {
	uint64_t k = 0u;
	assert(NUM_TILES == 16);
	for (int i = 0; i < NUM_TILES; ++i) {
		assert(board.state[i] < 16);
		k = (k << 4) | board.state[i];
	}
	return k;
}

#if 0 // unused
static void unpack_board_state(Board &board, const uint64_t state) {
	assert(NUM_TILES == 16);
	uint64_t k = state;
	for (int i = 0; i < 16; ++i) {
		board.state[15 - i] = (k & 0x0F);
		k >>= 4;
	}
}
#endif

static uint64_t mix64(uint64_t key) {
	// from: https://gist.github.com/badboy/6267743
	key = (~key) + (key << 21); // key = (key << 21) - key - 1;
	key = key ^ (key >> 24);
	key = key * 265;
	key = key ^ (key >> 14);
	key = key * 21;
	key = key ^ (key >> 28);
	key = key + (key << 31);
	return key;
}

#if CRAZY_VERBOSE_CACHE_DEBUGGER
template <typename T>
void print_blob(const T &v) {
	uint8_t buf[sizeof(T)];
	memcpy(buf, &v, sizeof(T));
	for (int i = 0; i < sizeof(T); ++i) { printf("%02x", buf[i]); }
}
#endif

template <typename T>
class BoardCache {
		enum {
			ENTRY_COUNT = (1 << 15),
			BUCKET_SIZE = 8,
			BUCKET_COUNT = ENTRY_COUNT / BUCKET_SIZE,
			BUCKET_INDEX_MASK = (BUCKET_COUNT - 1)
		};
		struct Bucket {
			uint64_t keys[BUCKET_SIZE];
			T values[BUCKET_SIZE];
		};

		const T *bucket_get(const Bucket &bucket, const uint64_t k) const {
			for (int i = 0; i < BUCKET_SIZE; ++i) {
				if (bucket.keys[i] == k) { return &bucket.values[i]; }
			}
			return 0;
		}

#if USE_CACHE_VERIFICATION_MAP
		typedef std::map<uint64_t, T> MapT;
		typedef typename MapT::iterator MapIterT;
		typedef typename MapT::value_type MapValueT;
#endif

	public:
		BoardCache(): m_buckets(0) {
			m_buckets = static_cast<Bucket*>(calloc(BUCKET_COUNT, sizeof(Bucket)));
		}

		~BoardCache() {
			free(m_buckets);
		}

		void reset() {
			memset(m_buckets, 0, BUCKET_COUNT * sizeof(Bucket));
		}

		void *where(const Board &board) { return where(pack_board_state(board)); }

		const void *where(const Board &board) const { return where(pack_board_state(board)); }

		void *where(const uint64_t k) {
			const uint64_t h = mix64(k);
			return static_cast<void*>(&m_buckets[h & BUCKET_INDEX_MASK]);
		}

		const void *where(const uint64_t k) const {
			const uint64_t h = mix64(k);
			return static_cast<const void*>(&m_buckets[h & BUCKET_INDEX_MASK]);
		}

		const T *get(const uint64_t k, const void *where) const {
			assert(k);
			assert(where);
			const Bucket &bucket = *static_cast<const Bucket*>(where);
			for (int i = 0; i < BUCKET_SIZE; ++i) {
				if (bucket.keys[i] == k) {
#if CRAZY_VERBOSE_CACHE_DEBUGGER
					printf("%016lx: get (found) ", k);
					print_blob(bucket.values[i]);
					printf(" : ");
					print_blob(m_verifier.find(k)->second);
					printf("\n");
#endif
#if USE_CACHE_VERIFICATION_MAP
					assert(m_verifier.count(k));
					assert(memcmp(&m_verifier.find(k)->second, &bucket.values[i], sizeof(T)) == 0);
#endif
					return &bucket.values[i];
				}
			}
#if CRAZY_VERBOSE_CACHE_DEBUGGER
			printf("%016lx: get (not found)\n", k);
#endif
			//assert(m_verifier.count(k) == 0); // not actually necessarily true
			return 0;
		}

		void put(const uint64_t k, void *where, const T &value) {
			assert(k);
			assert(where);
			Bucket &bucket = *static_cast<Bucket*>(where);
			for (int i = 0; i < BUCKET_SIZE; ++i) {
				if (bucket.keys[i] == k) {
#if CRAZY_VERBOSE_CACHE_DEBUGGER
					printf("%016lx: replace ", k);
					print_blob(bucket.values[i]);
					printf(" : ");
					print_blob(m_verifier.find(k)->second);
					printf("\n");
#endif
#if USE_CACHE_VERIFICATION_MAP
					assert(m_verifier.count(k));
					assert(memcmp(&m_verifier.find(k)->second, &bucket.values[i], sizeof(T)) == 0);
					m_verifier[k] = value;
#endif
					bucket.values[i] = value;
					return;
				}
			}
#if CRAZY_VERBOSE_CACHE_DEBUGGER
			printf("%016lx: put (new)\n", k);
#endif
			//assert(m_verifier.count(k) == 0); // not actually necessarily true
			for (int i = BUCKET_SIZE-1; i > 0; --i) {
				bucket.keys[i] = bucket.keys[i-1];
				bucket.values[i] = bucket.values[i-1];
			}
			bucket.keys[0] = k;
			bucket.values[0] = value;
#if USE_CACHE_VERIFICATION_MAP
			m_verifier[k] = value;
#endif
		}

		const T *get(const Board &board) const {
			const uint64_t k = pack_board_state(board);
			return get(k, where(k));
		}

		void put(const Board &board, const T &value) {
			const uint64_t k = pack_board_state(board);
			put(k, where(k), value);
		}

	private:
		Bucket *m_buckets;
#if USE_CACHE_VERIFICATION_MAP
		MapT m_verifier;
#endif
};

typedef int (*Evaluator)(const Board &board);

class Searcher {
	public:
		Searcher(): evalfn(0), num_moves(0), best_first_move(-1) {
			m_cancelled._nonatomic = 0;
		}

		void cancel() {
			mint_store_32_relaxed(&m_cancelled, 1);
		}

		int search(Evaluator evalfn, const Board &board, const RNG &rng, int lookahead) {
			assert(evalfn);
			this->evalfn = evalfn;
			this->num_moves = 0;
			this->best_first_move = -1;
			this->m_cancelled._nonatomic = 0;
			int move;
			int score = do_search(board, rng, lookahead, &move);
			if (this->m_cancelled._nonatomic) { return INT_MIN; }
			this->best_first_move = move;
			return score;
		}

		int get_num_moves() const { return num_moves; }
		int get_best_first_move() const { return best_first_move; }

	protected:
		int eval_board(const Board &board) { return evalfn(board); }
		void tally_move() { ++num_moves; }
		bool cancelled() { return (mint_load_32_relaxed(&m_cancelled) != 0); }

	private:
		Evaluator evalfn;
		int num_moves;
		int best_first_move;
		mint_atomic32_t m_cancelled;

		virtual int do_search(const Board &board, const RNG &rng, int lookahead, int *move) = 0;
};

class SearcherCheat : public Searcher {
	private:
		int do_search_real(const Board &board, const RNG &rng, int lookahead, int *move) {
			if (move) { *move = -1; }
			if (lookahead == 0) {
				if (cancelled()) { return INT_MIN; }
				return eval_board(board);
			}

			Board next_state;
			RNG next_rng;
			int best_score = INT_MIN;
			for (int i = 0; i < 4; ++i) {
				next_state = board;
				next_rng = rng;
				if (!next_state.move(i, 0, next_rng, 0)) { continue; } // ignore null moves
				tally_move();
				int score = do_search_real(next_state, next_rng, lookahead - 1, 0);
				if (cancelled()) { return INT_MIN; }
				if (score > best_score) {
					best_score = score;
					if (move) { *move = i; }
				}
			}
			return best_score;
		}

		virtual int do_search(const Board &board, const RNG &rng, int lookahead, int *move) {
			assert(lookahead >= 0);
			return do_search_real(board, rng, lookahead, move);
		}
};

class SearcherNaiveMinimax : public Searcher {
	private:
		int do_search_real(const Board &board, int lookahead, int *move) {
			if (move) { *move = -1; }
			if (lookahead == 0) {
				if (cancelled()) { return INT_MIN; }
				return eval_board(board);
			}

			int best_score;
			Board next_state;
			if (lookahead & 1) {
				// minimise
				best_score = INT_MAX;
				for (int i = 0; i < NUM_TILES; ++i) {
					if (board.state[i]) { continue; } // can only place tiles in empty cells
					for (int value = 1; value < 3; ++value) {
						next_state = board;
						next_state.state[i] = value;
						int score = do_search_real(next_state, lookahead - 1, 0);
						if (cancelled()) { return INT_MIN; }
						if (score < best_score) {
							best_score = score;
						}
					}
				}
			} else {
				// maximise
				best_score = INT_MIN;
				for (int i = 0; i < 4; ++i) {
					next_state = board;
					if (!next_state.tilt(DIR_DX[i], DIR_DY[i])) { continue; } // ignore null moves
					tally_move();
					int score = do_search_real(next_state, lookahead - 1, 0);
					if (cancelled()) { return INT_MIN; }
					if (score > best_score) {
						best_score = score;
						if (move) { *move = i; }
					}
				}
			}
			return best_score;
		}

		virtual int do_search(const Board &board, const RNG& /*rng*/, int lookahead, int *move) {
			assert(lookahead >= 0);
			return do_search_real(board, lookahead*2, move);
		}
};

class SearcherAlphaBeta : public Searcher {
	private:
		int num_pruned;

		int do_search_mini(const Board &board, int alpha, int beta, int lookahead) {
			Board next_state;
			for (int i = 0; i < NUM_TILES; ++i) {
				if (board.state[i]) { continue; } // can only place tiles in empty cells
				for (int value = 1; value < 3; ++value) {
					next_state = board;
					next_state.state[i] = value;
					beta = min(beta, do_search_maxi(next_state, alpha, beta, lookahead - 1, 0));
					if (cancelled()) { return INT_MAX; }
					if (alpha >= beta) { ++num_pruned; return beta; }
				}
			}
			return beta;
		}

		int do_search_maxi(const Board &board, int alpha, int beta, int lookahead, int *move) {
			if (move) { *move = -1; }
			if (lookahead == 0) {
				if (cancelled()) { return INT_MIN; }
				return eval_board(board);
			}
			// final score must be *at least* alpha and *at most* beta
			// alpha <= score <= beta
			Board next_state;
			for (int i = 0; i < 4; ++i) {
				next_state = board;
				if (!next_state.tilt(DIR_DX[i], DIR_DY[i])) { continue; } // ignore null moves
				tally_move();
				int score = do_search_mini(next_state, alpha, beta, lookahead - 1);
				if (cancelled()) { return INT_MIN; }
				if (score > alpha) {
					alpha = score;
					if (move) { *move = i; }
				}
				if (alpha >= beta) { ++num_pruned; return alpha; }
			}
			return alpha;
		}

		virtual int do_search(const Board &board, const RNG& /*rng*/, int lookahead, int *move) {
			assert(lookahead >= 0);
			num_pruned = 0;
			int score = do_search_maxi(board, INT_MIN, INT_MAX, lookahead*2, move);
#if PRINT_CACHE_STATS
			printf("(alpha-beta) alpha-beta pruned %d\n", num_pruned);
#endif
			return score;
		}
};

class SearcherCachingMinimax : public Searcher {
	private:
		struct Info { static const Info NIL; int lookahead; int score; };
		BoardCache<Info> cache;
		enum { STAT_DEPTH = 20 };
		int num_cached[STAT_DEPTH];

		void tally_cache_hit(int lookahead) {
			++num_cached[min(lookahead, STAT_DEPTH - 1)];
		}

		int do_search_real(const Board &board, int lookahead, int *move) {
			if (move) { *move = -1; }

			const uint64_t board_k = pack_board_state(board);
			void *cache_loc = cache.where(board_k);
			const Info *cached = cache.get(board_k, cache_loc);
			if (cached && cached->lookahead == lookahead) {
				tally_cache_hit(lookahead);
				return cached->score;
			}

			int best_score;

			if (lookahead == 0) {
				if (cancelled()) { return INT_MIN; }
				best_score = eval_board(board);
			} else {
				Board next_state;
				if (lookahead & 1) {
					// minimise
					best_score = INT_MAX;
					for (int i = 0; i < NUM_TILES; ++i) {
						if (board.state[i]) { continue; } // can only place tiles in empty cells
						for (int value = 1; value < 3; ++value) {
							next_state = board;
							next_state.state[i] = value;
							int score = do_search_real(next_state, lookahead - 1, 0);
							if (cancelled()) { return INT_MAX; }
							if (score < best_score) {
								best_score = score;
							}
						}
					}
				} else {
					// maximise
					best_score = INT_MIN;
					for (int i = 0; i < 4; ++i) {
						next_state = board;
						if (!next_state.tilt(DIR_DX[i], DIR_DY[i])) { continue; } // ignore null moves
						tally_move();
						int score = do_search_real(next_state, lookahead - 1, 0);
						if (cancelled()) { return INT_MIN; }
						if (score > best_score) {
							best_score = score;
							if (move) { *move = i; }
						}
					}
				}
			}

			const Info new_cached = { lookahead, best_score };
			cache.put(board_k, cache_loc, new_cached);
			return best_score;
		}

		virtual int do_search(const Board &board, const RNG& /*rng*/, int lookahead, int *move) {
			assert(lookahead >= 0);
			memset(num_cached, 0, sizeof(num_cached));
			cache.reset();
			int score = do_search_real(board, lookahead*2, move);
#if PRINT_CACHE_STATS
			printf("(caching-minimax) cache hits:");
			for (int i = 0; i < min(lookahead*2, STAT_DEPTH); ++i) { printf(" %d", num_cached[i]); }
			printf("\n");
#endif
			return score;
		}
};

const SearcherCachingMinimax::Info SearcherCachingMinimax::Info::NIL = { -1, INT_MIN };

class SearcherCachingAlphaBeta : public Searcher {
	private:
		enum { SCORE_UNKNOWN, SCORE_EXACT, SCORE_LOWER_BOUND, SCORE_UPPER_BOUND };
		struct Info { static const Info NIL; int16_t lookahead; int16_t type; int score; };
		BoardCache<Info> cache;
		enum { STAT_DEPTH = 20 };
		int num_cached[STAT_DEPTH];
		int num_pruned;

		void tally_cache_hit(int lookahead) {
			++num_cached[min(lookahead, STAT_DEPTH - 1)];
		}

		bool check_cached(const Info * const cached, int alpha, int beta, int lookahead, int &output) {
			bool cache_valid = false;
			if (cached && cached->lookahead == lookahead) {
				switch (cached->type) {
					case SCORE_EXACT: cache_valid = true; break;
					case SCORE_UPPER_BOUND: cache_valid = (cached->score <= alpha); break;
					case SCORE_LOWER_BOUND: cache_valid = (cached->score >= beta); break;
				}
			}
			if (cache_valid) {
				tally_cache_hit(lookahead);
				output = cached->score;
			}
			return cache_valid;
		}

		int do_search_mini(const Board &board, int alpha, int beta, int lookahead) {
			assert(alpha < beta);

			const uint64_t board_k = pack_board_state(board);
			void * const cache_loc = cache.where(board_k);

			const Info * const cached = cache.get(board_k, cache_loc);
			int cache_output;
			if (check_cached(cached, alpha, beta, lookahead, cache_output)) { return cache_output; }

			int cache_type = SCORE_LOWER_BOUND;
			Board next_state;
			for (int i = 0; i < NUM_TILES; ++i) {
				if (board.state[i]) { continue; } // can only place tiles in empty cells
				for (int value = 1; value < 3; ++value) {
					next_state = board;
					next_state.state[i] = value;
					int score = do_search_maxi(next_state, alpha, beta, lookahead - 1, 0);
					if (cancelled()) { return INT_MAX; }
					if (score < beta) {
						beta = score;
						cache_type = SCORE_EXACT;
					}
					if (alpha >= beta) {
						++num_pruned;
						cache_type = SCORE_UPPER_BOUND;
						goto prune;
					}
				}
			}
prune:
			const Info new_cached = { (int16_t)lookahead, (int16_t)cache_type, beta };
			cache.put(board_k, cache_loc, new_cached);
			return beta;
		}

		int do_search_maxi(const Board &board, int alpha, int beta, int lookahead, int *move) {
			if (move) { *move = -1; }
			assert(alpha < beta);

			const uint64_t board_k = pack_board_state(board);
			void * const cache_loc = cache.where(board_k);

			const Info * const cached = cache.get(board_k, cache_loc);
			int cache_output;
			if (check_cached(cached, alpha, beta, lookahead, cache_output)) { return cache_output; }

			if (lookahead == 0) {
				if (cancelled()) { return INT_MIN; }
				int score = eval_board(board);
				const Info new_cached = { 0, SCORE_EXACT, score };
				cache.put(board_k, cache_loc, new_cached);
				return score;
			} else {
				int cache_type = SCORE_UPPER_BOUND;
				Board next_state;
				for (int i = 0; i < 4; ++i) {
					next_state = board;
					if (!next_state.tilt(DIR_DX[i], DIR_DY[i])) { continue; } // ignore null moves
					tally_move();
					int score = do_search_mini(next_state, alpha, beta, lookahead - 1);
					if (cancelled()) { return INT_MIN; }
					if (score > alpha) {
						alpha = score;
						cache_type = SCORE_EXACT;
						if (move) { *move = i; }
					}
					if (alpha >= beta) {
						++num_pruned;
						cache_type = SCORE_LOWER_BOUND;
						goto prune;
					}
				}
prune:
				const Info new_cached = { (int16_t)lookahead, (int16_t)cache_type, alpha };
				cache.put(board_k, cache_loc, new_cached);
				return alpha;
			}
		}

		virtual int do_search(const Board &board, const RNG& /*rng*/, int lookahead, int *move) {
			assert(lookahead >= 0);
			memset(num_cached, 0, sizeof(num_cached));
			num_pruned = 0;
			cache.reset();
			int score = do_search_maxi(board, INT_MIN, INT_MAX, lookahead*2, move);
#if PRINT_CACHE_STATS
			printf("(caching-alpha-beta) alpha-beta pruned %d\n", num_pruned);
			printf("(caching-alpha-beta) cache hits:");
			for (int i = 0; i < min(lookahead*2, STAT_DEPTH); ++i) { printf(" %d", num_cached[i]); }
			printf("\n");
#endif
			return score;
		}
};

const SearcherCachingAlphaBeta::Info SearcherCachingAlphaBeta::Info::NIL = { -1, SCORE_UNKNOWN, INT_MIN };

static int monotonicity(const uint8_t *begin, int stride, int n) {
	int total = (n - 2);
	int i;
	for (i = 0; (*begin == 0) && i < n; ++i) { begin += stride; }
	int last_value = *begin, last_sign = 0;
	for (; i < n; ++i) {
		if (*begin) {
			const int delta = (*begin - last_value);
			const int sign = signum(delta);
			if (sign) {
				if (last_sign && last_sign != sign) { --total; }
				last_sign = sign;
			}
			last_value = *begin;
		}
		begin += stride;
	}
	return total;
}

static int ai_score_monotonicity(const Board &board) {
	int total = 0;
	// monotonicity of rows
	for (int i = 0; i < TILES_Y; ++i) {
		total += monotonicity(&board.state[i*TILES_X], 1, TILES_X);
	}
	// monotonicity of columns
	for (int j = 0; j < TILES_Y; ++j) {
		total += monotonicity(&board.state[j], TILES_X, TILES_Y);
	}
	return total;
}

static int ai_eval_board(const Board &board) {
	// try to maximise monotonicity
	return ai_score_monotonicity(board);
	// try to maximise free space
	//return board.count_free();
}

// -------- AI Worker Thread -------------------------------------------------------------------

class AIWorker {
	public:
		AIWorker();
		~AIWorker();

		void Cancel();
		void Reset(); // blocks
		void Work(const Board &board, const RNG &rng, int lookahead);
		bool IsWorking() const;
		bool IsDone(int *move = 0) const;
		void Wait(int *move = 0) const;

	private:
		SearcherCachingAlphaBeta m_searcher;
		Evaluator m_evalfn;
		int m_lookahead;

		Board m_board;
		RNG m_rng;

		tthread::thread m_thread;
		mutable tthread::mutex m_lock;
		mutable tthread::condition_variable m_trigger;
		bool m_working;
		bool m_done;
		int m_move;

		void Main();
		static void ai_worker_main(void *self);
};

AIWorker::AIWorker():
	m_evalfn(&ai_eval_board),
	m_lookahead(2),
	m_working(false), m_done(false), m_move(-1) {
	m_thread.start(&AIWorker::ai_worker_main, this);
}

AIWorker::~AIWorker() {}

void AIWorker::Work(const Board &board, const RNG &rng, int lookahead) {
	{
		tthread::lock_guard<tthread::mutex> guard(m_lock);
		if (m_working) {
			fprintf(stderr, "AIWorker::Work() called while we're already working!\n");
			return;
		}
		m_working = true;
		m_done = false;
		m_move = -1;
		m_board = board;
		m_rng = rng;
		m_lookahead = lookahead;
	}
	m_trigger.notify_one();
}

void AIWorker::Cancel() {
	m_searcher.cancel();
	Reset();
}

void AIWorker::Reset() {
	tthread::lock_guard<tthread::mutex> guard(m_lock);
	while (m_working) { m_trigger.wait(m_lock); }
	assert(!m_working);
	m_move = -1;
	m_done = false;
}

bool AIWorker::IsWorking() const {
	tthread::lock_guard<tthread::mutex> guard(m_lock);
	return m_working;
}

bool AIWorker::IsDone(int *move) const {
	tthread::lock_guard<tthread::mutex> guard(m_lock);
	if (move) { *move = (m_done ? m_move : -1); }
	return m_done;
}

void AIWorker::Wait(int *move) const {
	tthread::lock_guard<tthread::mutex> guard(m_lock);
	while (m_working) { m_trigger.wait(m_lock); }
	assert(!m_working);
	if (move && m_done) { *move = m_move; }
}

void AIWorker::Main() {
	Board board;
	RNG rng;
	int lookahead;
	while (true) {
		{
			tthread::lock_guard<tthread::mutex> guard(m_lock);
			while (!m_working) {
				m_trigger.wait(m_lock);
			}

			board = m_board;
			rng = m_rng;
			lookahead = m_lookahead;
		}

		m_searcher.search(m_evalfn, board, rng, lookahead);
		int move = m_searcher.get_best_first_move();
#if 1 || PRINT_CACHE_STATS
		printf("tried %d moves!\n", m_searcher.get_num_moves());
#endif

		{
			tthread::lock_guard<tthread::mutex> guard(m_lock);
			m_move = move;
			m_done = true;
			m_working = false;
		}
		m_trigger.notify_one();
	}
}

void AIWorker::ai_worker_main(void *self) { static_cast<AIWorker*>(self)->Main(); }

#if 1

#else

static bool automove(BoardHistory &history, AnimState &anim) {
	const int lookahead = 3;

	//SearcherCheat searcher;
	SearcherNaiveMinimax searcher_a;
	SearcherAlphaBeta searcher_b;
	SearcherCachingMinimax searcher_c;
	SearcherCachingAlphaBeta searcher_d;

	const Board &board = history.get();
	const RNG &rng = history.get_rng();
#if PRINT_BOARD_STATE
	printf("AI move, board state: %016lx rng %08x,%08x,%08x,%08x (lookahead = %d)\n",
			pack_board_state(board), rng.x, rng.y, rng.z, rng.w, lookahead);
#endif

	int move_a = ai_move(searcher_a, &ai_eval_board, board, rng, lookahead);
	int move_b = ai_move(searcher_b, &ai_eval_board, board, rng, lookahead);
	assert(move_a == move_b);
	int move_c = ai_move(searcher_c, &ai_eval_board, board, rng, lookahead);
	assert(move_a == move_c);
	int move_d = ai_move(searcher_d, &ai_eval_board, board, rng, lookahead);
	assert(move_a == move_d);

	int move = move_a;
	if (move != -1) {
		history.move(move, anim);
		return true;
	} else {
		return false;
	}
}
#endif

static void render_rounded_square(float x, float y, float extent, float rounding) {
	assert(rounding >= 0.0f);
	assert(extent >= rounding);
	const float inner_extent = extent - rounding;
	glBegin(GL_TRIANGLES);
	if (inner_extent > 0.0f) {
		glVertex2f(x - inner_extent, y - extent); // top-left
		glVertex2f(x - inner_extent, y + extent);
		glVertex2f(x + inner_extent, y - extent);
		glVertex2f(x - inner_extent, y + extent); // bottom-right
		glVertex2f(x + inner_extent, y + extent);
		glVertex2f(x + inner_extent, y - extent);
	}
	if (rounding > 0.0f) {
		if (inner_extent > 0.0f) {
			glVertex2f(x - extent,       y - inner_extent); // top-left
			glVertex2f(x - extent,       y + inner_extent);
			glVertex2f(x - inner_extent, y - inner_extent);
			glVertex2f(x - extent,       y + inner_extent); // bottom-right
			glVertex2f(x - inner_extent, y + inner_extent);
			glVertex2f(x - inner_extent, y - inner_extent);

			glVertex2f(x + inner_extent, y - inner_extent); // top-left
			glVertex2f(x + inner_extent, y + inner_extent);
			glVertex2f(x + extent,       y - inner_extent);
			glVertex2f(x + inner_extent, y + inner_extent); // bottom-right
			glVertex2f(x + extent,       y + inner_extent);
			glVertex2f(x + extent,       y - inner_extent);
		}

		float dx = rounding, dy = 0.0f;
		int nsegments = 7;
		for (int i = 0; i < 4; ++i) {
			const float cx = x + ((i == 0 || i == 3) ? inner_extent : -inner_extent);
			const float cy = y + ((i & 2) ? inner_extent : -inner_extent);
			for (int j = 0; j < nsegments; ++j) {
				const int segment = i*nsegments + j;
				const float angle = (float)(segment+1) * ((2*(float)M_PI) / (4*nsegments));
				glVertex2f(cx, cy);
				glVertex2f(cx + dx, cy + dy);
				dx = rounding * cosf(angle);
				dy = rounding * -sinf(angle);
				glVertex2f(cx + dx, cy + dy);
			}
		}
	}
	glEnd();
}

// -------- GLOBAL GAME CONFIGURATION/STYLE ----------------------------------------------------

// speeds are multiplied by 1000 so that the animation key frame time values can be specified in milliseconds
static const double ANIM_SPEED_NORMAL = 1.0 * 1000.0;
static const double ANIM_SPEED_AUTOPLAY = 2.0 * 1000.0;

static const uint8_t TILE_COLORS[16][4] = {
	{ 211, 199, 187, 255 }, // blank tile
	{ 238, 228, 218, 255 }, //     2
	{ 237, 224, 200, 255 }, //     4
	{ 242, 177, 121, 255 }, //     8

	{ 245, 149,  99, 255 }, //    16
	{ 246, 124,  95, 255 }, //    32
	{ 246,  94,  59, 255 }, //    64
	{ 237, 207, 114, 255 }, //   128

	{ 237, 204,  97, 255 }, //   256
	{ 237, 200,  80, 255 }, //   512
	{ 237, 197,  63, 255 }, //  1024
	{ 237, 194,  46, 255 }, //  2048

	{ 206, 234,  49, 255 }, //  4096
	{ 188, 234,  49, 255 }, //  8192
	{ 171, 234,  49, 255 }, // 16384
	{ 153, 234,  49, 255 }  // 32768
};

static const uint8_t TILE_TEXT_COLORS[16][4] = {
	{ 255, 255,   0, 255 }, // blank tile
	{ 119, 110, 101, 255 }, //     2
	{ 119, 110, 101, 255 }, //     4
	{ 249, 246, 242, 255 }, //     8

	{ 249, 246, 242, 255 }, //    16
	{ 249, 246, 242, 255 }, //    32
	{ 249, 246, 242, 255 }, //    64
	{ 249, 246, 242, 255 }, //   128

	{ 249, 246, 242, 255 }, //   256
	{ 249, 246, 242, 255 }, //   512
	{ 249, 246, 242, 255 }, //  1024
	{ 249, 246, 242, 255 }, //  2048

	{ 119, 110, 101, 255 }, //  4096
	{ 119, 110, 101, 255 }, //  8192
	{ 119, 110, 101, 255 }, // 16384
	{ 119, 110, 101, 255 }  // 32768
};

static const char *TILE_TEXT[16] = {
	"",
	"2",
	"4",
	"8",
	"16",
	"32",
	"64",
	"128",
	"256",
	"512",
	"1024",
	"2048",
	"4096",
	"8192",
	"16384",
	"32768"
};

static const float TILE_EXTENT = 64.0f - 6.0f;
static const float TILE_ROUNDING = 4.0f;
static const float TILE_FONT_SIZE = 50.0f;

static const float BOARD_EXTENT = 256.0f + 6.0f;
static const float BOARD_ROUNDING = 6.0f;

static const uint8_t MESSAGE_TEXT_COLOR[4] = { 119, 110, 101, 255 };
static const float MESSAGE_FONT_SIZE = 36.0f;

static const uint8_t PLUS_SCORE_TEXT_COLOR[4] = { 141, 217, 65, 255 };
static const float PLUS_SCORE_FONT_SIZE = 70.0f;

static const uint8_t HELP_BG_COLOR[4] = { 220, 218, 202, 255};
static const uint8_t HELP_KEY_COLOR[4] = { 132, 76, 43, 255 };
static const uint8_t HELP_KEY_TEXT_COLOR[4] = { 239, 223, 188, 255 };
static const uint8_t HELP_TEXT_COLOR[4] = { 53, 40, 23, 255 };
static const float HELP_KEY_X = -128.0f;
static const float HELP_TEXT_X = 64.0f;
static const float HELP_KEY_EXTENT = 16.0f;
static const float HELP_KEY_ROUNDING = 3.0f;
static const float HELP_TITLE_FONT_SIZE = 42.0f;
static const float HELP_TEXT_FONT_SIZE = 32.0f;
static const float HELP_KEY_FONT_SIZE = 28.0f;

// -------- GLOBAL STATE -----------------------------------------------------------------------

static FONScontext *fons;
static int font;

static BoardHistory s_history;
static AnimState s_anim;
static double s_anim_time0;
static double s_anim_speed;
static bool s_autoplay;

static AIWorker *s_ai_worker;

static void render_tile(int value, float x, float y, float scale) {
	assert(value >= 0 && value < 16);
	const uint8_t *col = TILE_COLORS[value];
	const uint8_t *text_col = TILE_TEXT_COLORS[value];
	const char *text = TILE_TEXT[value];

	glPushMatrix();
	glTranslatef(x + 64.0f, y + 64.0f, 0.0f);
	glScalef(scale, scale, 1.0f);

	glDisable(GL_TEXTURE_2D);
	glColor4ub(col[0], col[1], col[2], col[3]);
	render_rounded_square(0.0f, 0.0f, TILE_EXTENT, TILE_ROUNDING);

	if (value > 0) {
		glEnable(GL_TEXTURE_2D);
		fonsClearState(fons);
		fonsSetAlign(fons, FONS_ALIGN_CENTER | FONS_ALIGN_MIDDLE);
		fonsSetSize(fons, TILE_FONT_SIZE);
		fonsSetColor(fons, glfonsRGBA(text_col[0], text_col[1], text_col[2], text_col[3]));
		fonsSetFont(fons, font);
		fonsDrawText(fons, 0.0f, 0.0f, text, 0);
	}

	glPopMatrix();
}

static void render_tiles_anim(float t, const Board& /*board*/, const AnimState &anim) {
	for (int i = 0; i < anim.ntiles; ++i) {
		const TileAnim &tile = anim.tiles[i];
		render_tile(tile.value, tile.x.eval(t), tile.y.eval(t), tile.scale.eval(t));
	}
}

static void render_scores_anim(float t, const AnimState &anim) {
	const uint8_t * const text_col = PLUS_SCORE_TEXT_COLOR;
	char buf[32];
	glEnable(GL_TEXTURE_2D);
	for (int i = 0; i < anim.nscores; ++i) {
		const ScoreAnim &score = anim.scores[i];

		const float alpha = score.alpha.eval(t);
		if (alpha > 0.0f) {
			snprintf(buf, sizeof(buf), "+%d", score.score);

			fonsClearState(fons);
			fonsSetAlign(fons, FONS_ALIGN_CENTER | FONS_ALIGN_MIDDLE);
			fonsSetSize(fons, PLUS_SCORE_FONT_SIZE);
			fonsSetColor(fons, glfonsRGBA(text_col[0], text_col[1], text_col[2], (uint8_t)(alpha*255.0f)));
			fonsSetFont(fons, font);
			fonsDrawText(fons, score.x.eval(t) + 64.0f, score.y.eval(t) + 64.0f, buf, 0);
		}
	}
}

static void render_tiles_static(const Board &board) {
	for (int i = 0; i < NUM_TILES; ++i) {
		const int value = board.state[i];
		if (value) {
			float x, y;
			tile_idx_to_xy(i, &x, &y);
			render_tile(value, x, y, 1.0f);
		}
	}
}

static void render_score(int score) {
	char buf[32];
	snprintf(buf, sizeof(buf), "%d", score);

	fonsClearState(fons);
	fonsSetAlign(fons, FONS_ALIGN_RIGHT | FONS_ALIGN_BASELINE);
	fonsSetSize(fons, MESSAGE_FONT_SIZE);
	const uint8_t *c = MESSAGE_TEXT_COLOR;
	fonsSetColor(fons, glfonsRGBA(c[0], c[1], c[2], c[3]));
	fonsSetFont(fons, font);
	fonsDrawText(fons, 0.0f, 0.0f, buf, 0);
}

static void render(int wnd_w, int wnd_h, float t, const Board &board, const AnimState &anim) {
	glClearColor(250/255.0f, 248/255.0f, 239/255.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	glViewport(0, 0, wnd_w, wnd_h);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0.0, (double)wnd_w, (double)wnd_h, 0.0, -1.0, 1.0);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	glDisable(GL_TEXTURE_2D);
	glColor4ub(187, 173, 160, 255);
	render_rounded_square(wnd_w/2, wnd_h/2, BOARD_EXTENT, BOARD_ROUNDING);

	glTranslatef((wnd_w/2) - 256.0f, (wnd_h/2) - 256.0f, 0.0f);

	if (t <= anim.period) {
		render_tiles_anim(t, board, anim);
		render_scores_anim(t, anim);
	} else {
		render_tiles_static(board);
	}

	glLoadIdentity();
	glTranslatef(wnd_w*0.5f + 256.0f, wnd_h * 0.5f - 256.0f - 32.0f, 0.0f);
	render_score(s_history.get_score());

	if (s_ai_worker->IsWorking()) {
		glLoadIdentity();
		glEnable(GL_TEXTURE_2D);
		fonsClearState(fons);
		fonsSetAlign(fons, FONS_ALIGN_CENTER | FONS_ALIGN_BASELINE);
		fonsSetSize(fons, MESSAGE_FONT_SIZE);
		const uint8_t *c = MESSAGE_TEXT_COLOR;
		fonsSetColor(fons, glfonsRGBA(c[0], c[1], c[2], c[3]));
		fonsSetFont(fons, font);
		fonsDrawText(fons, wnd_w/2, wnd_h - 20.0f, "thinking...", 0);
	}

#if 0
	glLoadIdentity();
	fonsDrawDebug(fons, 0.0f, 0.0f, 0xff000000);
#endif
}

static void draw_text_multiline(FONScontext *fons, float x, float y, const char *text, int align) {
	float ascender, descender, line_height;
	fonsVertMetrics(fons, &ascender, &descender, &line_height);

	int nlines = 1;
	for (const char *c = text; *c; ++c) { if (*c == '\n') { ++nlines; } }

	float total_height = (nlines - 1)*line_height + ascender - descender;
	if (align & FONS_ALIGN_TOP) { y += ascender; }
	else if (align & FONS_ALIGN_BOTTOM) { y += ascender - total_height; }
	else if (align & FONS_ALIGN_MIDDLE) { y += ascender - 0.5f * total_height; }

	fonsPushState(fons);
	fonsSetAlign(fons, (align & FONS_MASK_HALIGN) | FONS_ALIGN_BASELINE);

	for (const char *c = text; *c; ++c) {
		if (*c == '\n') {
			if (c != text) { fonsDrawText(fons, x, y, text, c); }
			text = c + 1;
			y += line_height;
		}
	}
	fonsDrawText(fons, x, y, text, 0);

	fonsPopState(fons);
}

static void draw_arrow(float x, float y, int dir) {
	const float stem_halfwidth = 1.0f;
	const float stem_halflength = 9.0f;
	const float arrow_length = 8.0f;

	glPushMatrix();
	glTranslatef(x, y, 0.0f);

	switch (dir) {
		case MOVE_LEFT: glRotatef(90.0f, 0.0f, 0.0f, -1.0f); break;
		case MOVE_RIGHT: glRotatef(90.0f, 0.0f, 0.0f, 1.0f); break;
		case MOVE_UP: break;
		case MOVE_DOWN: glRotatef(180.0f, 0.0f, 0.0f, 1.0f); break;
	}

	glBegin(GL_TRIANGLES);
	glVertex2f(0.0f, -stem_halflength);
	glVertex2f(- 0.5f*arrow_length, - stem_halflength + arrow_length);
	glVertex2f(+ 0.5f*arrow_length, - stem_halflength + arrow_length);
	glVertex2f(+ stem_halfwidth, - stem_halflength + arrow_length);
	glVertex2f(- stem_halfwidth, - stem_halflength + arrow_length);
	glVertex2f(- stem_halfwidth, + stem_halflength);
	glVertex2f(+ stem_halfwidth, - stem_halflength + arrow_length);
	glVertex2f(- stem_halfwidth, + stem_halflength);
	glVertex2f(+ stem_halfwidth, + stem_halflength);
	glEnd();

	glPopMatrix();
}

static void render_help(int wnd_w, int wnd_h) {
	const uint8_t *c;
	glClearColor(250/255.0f, 248/255.0f, 239/255.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	glViewport(0, 0, wnd_w, wnd_h);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0.0, (double)wnd_w, (double)wnd_h, 0.0, -1.0, 1.0);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	glDisable(GL_TEXTURE_2D);
	c = HELP_BG_COLOR;
	glColor4ub(c[0], c[1], c[2], c[3]);
	render_rounded_square(wnd_w/2, wnd_h/2, BOARD_EXTENT, BOARD_ROUNDING);

	glTranslatef((wnd_w/2), (wnd_h/2) - 256.0f, 0.0f);

	fonsClearState(fons);
	fonsSetFont(fons, font);
	fonsSetAlign(fons, FONS_ALIGN_CENTER | FONS_ALIGN_MIDDLE);
	fonsSetSize(fons, HELP_KEY_FONT_SIZE);
	c = HELP_KEY_TEXT_COLOR;
	fonsSetColor(fons, glfonsRGBA(c[0], c[1], c[2], c[3]));

	float x, y;
	x = HELP_KEY_X;
	y = 170.0f - (HELP_KEY_EXTENT + 2.0f);
	glColor4ub(HELP_KEY_COLOR[0], HELP_KEY_COLOR[1], HELP_KEY_COLOR[2], HELP_KEY_COLOR[3]);
	render_rounded_square(x, y, HELP_KEY_EXTENT, HELP_KEY_ROUNDING);
	glColor4ub(HELP_KEY_TEXT_COLOR[0], HELP_KEY_TEXT_COLOR[1], HELP_KEY_TEXT_COLOR[2], HELP_KEY_TEXT_COLOR[3]);
	draw_arrow(x, y, MOVE_UP);
	y = 170.0f + (HELP_KEY_EXTENT + 2.0f);
	x = HELP_KEY_X - (2.0f*HELP_KEY_EXTENT + 4.0f);
	glColor4ub(HELP_KEY_COLOR[0], HELP_KEY_COLOR[1], HELP_KEY_COLOR[2], HELP_KEY_COLOR[3]);
	render_rounded_square(x, y, HELP_KEY_EXTENT, HELP_KEY_ROUNDING);
	glColor4ub(HELP_KEY_TEXT_COLOR[0], HELP_KEY_TEXT_COLOR[1], HELP_KEY_TEXT_COLOR[2], HELP_KEY_TEXT_COLOR[3]);
	draw_arrow(x, y, MOVE_LEFT);
	x = HELP_KEY_X;
	glColor4ub(HELP_KEY_COLOR[0], HELP_KEY_COLOR[1], HELP_KEY_COLOR[2], HELP_KEY_COLOR[3]);
	render_rounded_square(x, y, HELP_KEY_EXTENT, HELP_KEY_ROUNDING);
	glColor4ub(HELP_KEY_TEXT_COLOR[0], HELP_KEY_TEXT_COLOR[1], HELP_KEY_TEXT_COLOR[2], HELP_KEY_TEXT_COLOR[3]);
	draw_arrow(x, y, MOVE_DOWN);
	x = HELP_KEY_X + (2.0f*HELP_KEY_EXTENT + 4.0f);
	glColor4ub(HELP_KEY_COLOR[0], HELP_KEY_COLOR[1], HELP_KEY_COLOR[2], HELP_KEY_COLOR[3]);
	render_rounded_square(x, y, HELP_KEY_EXTENT, HELP_KEY_ROUNDING);
	glColor4ub(HELP_KEY_TEXT_COLOR[0], HELP_KEY_TEXT_COLOR[1], HELP_KEY_TEXT_COLOR[2], HELP_KEY_TEXT_COLOR[3]);
	draw_arrow(x, y, MOVE_RIGHT);

	glColor4ub(HELP_KEY_COLOR[0], HELP_KEY_COLOR[1], HELP_KEY_COLOR[2], HELP_KEY_COLOR[3]);

	y = 260.0f;
	x = HELP_KEY_X - (HELP_KEY_EXTENT + 2.0f);
	render_rounded_square(x, y, HELP_KEY_EXTENT, HELP_KEY_ROUNDING);
	fonsDrawText(fons, x, y, "Z", 0);
	glDisable(GL_TEXTURE_2D);
	x = HELP_KEY_X + (HELP_KEY_EXTENT + 2.0f);
	render_rounded_square(x, y, HELP_KEY_EXTENT, HELP_KEY_ROUNDING);
	fonsDrawText(fons, x, y, "X", 0);
	glDisable(GL_TEXTURE_2D);

	y = 350.0f;
	x = HELP_KEY_X;
	render_rounded_square(x, y, HELP_KEY_EXTENT, HELP_KEY_ROUNDING);
	fonsDrawText(fons, x, y, "H", 0);
	glDisable(GL_TEXTURE_2D);

	y = 440.0f;
	x = HELP_KEY_X;
	render_rounded_square(x, y, HELP_KEY_EXTENT, HELP_KEY_ROUNDING);
	fonsDrawText(fons, x, y, "P", 0);

	const int align = FONS_ALIGN_CENTER | FONS_ALIGN_MIDDLE;
	fonsClearState(fons);
	fonsSetFont(fons, font);
	fonsSetAlign(fons, align);
	fonsSetSize(fons, HELP_TITLE_FONT_SIZE);
	c = HELP_TEXT_COLOR;
	fonsSetColor(fons, glfonsRGBA(c[0], c[1], c[2], c[3]));
	fonsDrawText(fons, 0.0f, 50.0f, "Try to reach the 2048 tile!", 0);
	fonsSetSize(fons, HELP_TEXT_FONT_SIZE);
	draw_text_multiline(fons, HELP_TEXT_X, 170.0f, "Use the arrow keys to\nmove the tiles", align);
	draw_text_multiline(fons, HELP_TEXT_X, 260.0f, "Use Z and A to\nundo and redo", align);
	draw_text_multiline(fons, HELP_TEXT_X, 350.0f, "Use H to get a hint\nfrom the computer", align);
	draw_text_multiline(fons, HELP_TEXT_X, 440.0f, "Use P to toggle\nauto-play", align);

#if 0
	glLoadIdentity();
	fonsDrawDebug(fons, 0.0f, 0.0f, 0xff000000);
#endif
}

static void stop_anim() {
	s_anim.reset();
	s_anim_time0 = 0.0;
	s_anim_speed = 1.0;
}

static void start_anim(const double speed) {
	assert(speed > 0.0);
	if (s_anim.tiles_changed()) {
		s_anim_time0 = glfwGetTime();
		s_anim_speed = speed;
	} else {
		stop_anim();
	}
}

static void automove() {
	assert(s_ai_worker);
	const int lookahead = 5;
	s_ai_worker->Work(s_history.get(), s_history.get_rng(), lookahead);
}

static void handle_key(GLFWwindow * /*wnd*/, int key, int /*scancode*/, int action, int /*mods*/) {
	if (action == GLFW_PRESS) {
		if (key == GLFW_KEY_ESCAPE) {
			exit(0);
		} else {
			if (s_autoplay) {
				if (key == GLFW_KEY_P) {
					s_ai_worker->Cancel();
					s_autoplay = false;
				}
			} else {
				if (s_ai_worker->IsWorking()) { return; }

				stop_anim();
				switch (key) {
					case GLFW_KEY_RIGHT: { s_history.move(MOVE_RIGHT, s_anim); } break;
					case GLFW_KEY_LEFT:  { s_history.move(MOVE_LEFT, s_anim); } break;
					case GLFW_KEY_DOWN:  { s_history.move(MOVE_DOWN, s_anim); } break;
					case GLFW_KEY_UP:    { s_history.move(MOVE_UP, s_anim); } break;
					case GLFW_KEY_Z:     { s_history.undo(); } break;
					case GLFW_KEY_X:     { s_history.redo(); } break;
					case GLFW_KEY_N:     { s_history.new_game(s_anim); } break;
					case GLFW_KEY_H:     { automove(); } break;
					case GLFW_KEY_P:     { s_autoplay = true; automove(); } break;
				}
				start_anim(ANIM_SPEED_NORMAL);
			}
		}
	}
}

static void prime_fontstash_cache(GLFWwindow *wnd) {
	// hack to cache glyphs we know we'll need
	// (text is positioned off screen)
	int wnd_w, wnd_h;
	glfwGetFramebufferSize(wnd, &wnd_w, &wnd_h);
	glViewport(0, 0, wnd_w, wnd_h);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0.0, wnd_w, wnd_h, 0.0, -1.0, 1.0);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	fonsClearState(fons);
	fonsSetFont(fons, font);
	fonsSetSize(fons, PLUS_SCORE_FONT_SIZE);
	fonsDrawText(fons, 0.0f, -50.0f, "0123456789+", 0);
	fonsSetSize(fons, TILE_FONT_SIZE);
	fonsDrawText(fons, 0.0f, -50.0f, "0123456789", 0); // tile numbers
	fonsSetSize(fons, MESSAGE_FONT_SIZE);
	fonsDrawText(fons, 0.0f, -50.0f, "0123456789", 0); // score numbers
	fonsDrawText(fons, 0.0f, -50.0f, "thinking...", 0); // message(s)
}

static void fons_error_callback(void* /*udata*/, int error, int /*val*/) {
	switch (error) {
		case FONS_ATLAS_FULL: fprintf(stderr, "font-stash atlas is full!\n"); break;
		case FONS_SCRATCH_FULL: fprintf(stderr, "font-stash scratch buffer is full!\n"); break;
		case FONS_STATES_OVERFLOW: fprintf(stderr, "font-stash state stack overflow!\n"); break;
		case FONS_STATES_UNDERFLOW: fprintf(stderr, "font-stash state stack underflow!\n"); break;
		default: assert(0 && "unknown font-stash error code"); break;
	}
}

int main(int /*argc*/, char** /*argv*/) {
	glfwInit();
	glfwWindowHint(GLFW_SAMPLES, 8);
	glfwWindowHint(GLFW_DEPTH_BITS, 0);
	GLFWwindow *wnd = glfwCreateWindow(700, 700, "2048", NULL, NULL);

	glfwMakeContextCurrent(wnd);
	glDisable(GL_DEPTH_TEST);
	glEnable(GL_CULL_FACE);

	fons = glfonsCreate(256, 256, FONS_ZERO_TOPLEFT);
	fonsSetErrorCallback(fons, &fons_error_callback, 0);
	font = fonsAddFont(fons, "clearsans", "ClearSans-Bold.ttf");
	if (font == FONS_INVALID) {
		fprintf(stderr, "could not load font 'ClearSans-Bold.ttf'");
		return 1;
	}

	prime_fontstash_cache(wnd);

	s_ai_worker = new AIWorker();

	s_autoplay = false;
	s_anim.reset();
	s_history.reset();

#if 0
	{
		Board board;
		RNG rng;
		unpack_board_state(board, 0x7100630035102200ul);
		rng.x = 0xdec687c8u;
		rng.y = 0x2c30e98bu;
		rng.z = 0xa20ee555u;
		rng.w = 0x8587a82eu;
		s_history.reset(board, rng);
		s_anim.reset();
		start_anim(0.0);
	}
#else
	s_history.new_game(s_anim);
	start_anim(ANIM_SPEED_NORMAL);
#endif

	glfwSetKeyCallback(wnd, &handle_key);

	while (!glfwWindowShouldClose(wnd)) {
		const double t = max(0.0, (glfwGetTime() - s_anim_time0)) * s_anim_speed;
		const bool anim_done = (t >= s_anim.period);

		int wnd_w, wnd_h;
		glfwGetFramebufferSize(wnd, &wnd_w, &wnd_h);
		render(wnd_w, wnd_h, t, s_history.get(), s_anim);
		glfwSwapBuffers(wnd);

		if (anim_done) {
			stop_anim();
			int move;
			if (s_ai_worker->IsDone(&move)) {
				// clear the worker so we don't get triggered for this move again
				s_ai_worker->Reset();
				if (move == -1) {
					// force autoplay off if the AI gets stuck or has nothing to work on
					s_autoplay = false;
				} else {
					s_anim.reset();
					s_history.move(move, s_anim);
					start_anim(s_autoplay ? ANIM_SPEED_AUTOPLAY : ANIM_SPEED_NORMAL);

					if (s_autoplay) {
						// when autoplaying, overlap computation of the
						// next move with animation of the last move
						automove();
					}
				}
			}
		}

		// if we're not animating then be nice and don't spam the CPU & GPU
		// (note: we don't check anim_done here, because that value is out of date!)
		if (!s_anim.tiles_changed() && !s_ai_worker->IsWorking()) {
			glfwWaitEvents();
		} else {
			glfwPollEvents();
		}
	}
	glfwTerminate();
	return 0;
}

// vim: set ts=4 sw=4 noet:
