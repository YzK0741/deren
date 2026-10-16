#pragma once
#if !defined(BOOST_NO_EXCEPTIONS) || !defined(BOOST_ALL_NO_LIB)
#error Boost.Graph requires the private no-exceptions target
#endif
#if defined(BOOST_GRAPH_USE_MPI)
#error Distributed Boost.Graph is not included
#endif
#include <algorithm>
#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/breadth_first_search.hpp>
#include <boost/graph/filtered_graph.hpp>
#include <boost/graph/reverse_graph.hpp>
#include <boost/version.hpp>
#include <functional>
#include <span>
static_assert(BOOST_VERSION == 109200);
