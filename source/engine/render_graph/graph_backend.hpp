#pragma once
// Include after utility import; graph_backend_headers.hpp belongs in the global fragment.
struct vessel_vecS {};
struct vessel_listS {};
namespace boost {
    template <class T>
    struct container_gen<vessel_vecS, T> {
        using type = vessel::vector<T>;
    };
    template <class T>
    struct container_gen<vessel_listS, T> {
        using type = vessel::list<T>;
    };
    template <>
    struct parallel_edge_traits<vessel_vecS> {
        using type = allow_parallel_edge_tag;
    };
    namespace detail {
        template <>
        struct is_random_access<vessel_vecS> {
            enum { value = true };
            using type = mpl::true_;
        };
    } // namespace detail
} // namespace boost
struct graph_properties {
    static void* operator new(std::size_t n) noexcept {
        return mem::allocate(n, alignof(graph_properties));
    }
    static void operator delete(void* p) noexcept {
        mem::deallocate(p);
    }
};
using graph = boost::adjacency_list<vessel_vecS, vessel_vecS, boost::bidirectionalS,
                                    boost::no_property, boost::no_property, graph_properties, vessel_listS>;
using vertex = boost::graph_traits<graph>::vertex_descriptor;
struct queue {
    vessel::vector<vertex> data;
    std::size_t head = 0;
    void push(vertex v) {
        data.push_back(v);
    }
    void pop() {
        ++head;
    }
    vertex top() const {
        return data[head];
    }
    bool empty() const {
        return head == data.size();
    }
};
static vessel::vector<boost::default_color_type> reachable(graph const& g, std::span<vertex const> roots) {
    vessel::vector<boost::default_color_type> colors(boost::num_vertices(g), boost::white_color);
    auto reversed = boost::make_reverse_graph(g);
    auto map = boost::make_iterator_property_map(colors.begin(), boost::get(boost::vertex_index, g));
    queue pending;
    for (auto root : roots) {
        if (colors[root] == boost::white_color)
            boost::breadth_first_visit(reversed, root, pending, boost::default_bfs_visitor{}, map);
    }
    return colors;
}
struct live_vertex {
    boost::default_color_type const* colors = nullptr;
    bool operator()(vertex v) const {
        return colors[v] != boost::white_color;
    }
};
enum class error { cycle };
// Project policy: choose the smallest original pass index among all ready passes.
static std::expected<vessel::vector<vertex>, error> stable_order(graph const& g, live_vertex live, vertex* blocked = nullptr) {
    auto view = boost::make_filtered_graph(g, boost::keep_all{}, live);
    vessel::vector<std::size_t> degrees(boost::num_vertices(g), 0);
    vessel::vector<vertex> ready, result;
    std::size_t count = 0;
    for (auto [i, end] = boost::vertices(view); i != end; ++i) {
        ++count;
        degrees[*i] = boost::in_degree(*i, view);
        if (degrees[*i] == 0)
            ready.push_back(*i);
    }
    std::make_heap(ready.begin(), ready.end(), std::greater<vertex>{});
    while (!ready.empty()) {
        std::pop_heap(ready.begin(), ready.end(), std::greater<vertex>{});
        auto u = ready.back();
        ready.pop_back();
        result.push_back(u);
        for (auto [i, end] = boost::out_edges(u, view); i != end; ++i) {
            auto v = boost::target(*i, view);
            if (--degrees[v] == 0) {
                ready.push_back(v);
                std::push_heap(ready.begin(), ready.end(), std::greater<vertex>{});
            }
        }
    }
    if (result.size() != count) {
        if (blocked) {
            for (vertex v = 0; v < degrees.size(); ++v) {
                if (live(v) && degrees[v] != 0) {
                    *blocked = v;
                    break;
                }
            }
        }
        return std::unexpected(error::cycle);
    }
    return result;
}
