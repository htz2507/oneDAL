/*******************************************************************************
* Copyright 2020 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#pragma once

#include <algorithm>
#include <fstream>

#include "oneapi/dal/exceptions.hpp"
#include "oneapi/dal/graph/detail/graph_container.hpp"
#include "oneapi/dal/graph/detail/undirected_adjacency_array_graph_impl.hpp"
#include "oneapi/dal/graph/graph_common.hpp"
#include "oneapi/dal/graph/undirected_adjacency_array_graph.hpp"
#include "oneapi/dal/io/detail/load_graph_service.hpp"
#include "oneapi/dal/detail/threading.hpp"
#include "oneapi/dal/io/graph_csv_data_source.hpp"
#include "oneapi/dal/io/load_graph_descriptor.hpp"
#include "services/daal_atomic_int.h"
#include "services/daal_memory.h"

namespace oneapi::dal::preview::load_graph::detail {
edge_list<std::int32_t> load_edge_list(const std::string &name) {
    using int_t = std::int32_t;

    std::ifstream file(name);
    if (!file.is_open()) {
        throw invalid_argument("File not found");
    }
    edge_list<int_t> elist;
    elist.reserve(1024);

    char source_vertex[32], destination_vertex[32];
    while (file >> source_vertex >> destination_vertex) {
        auto edge = std::make_pair(daal_string_to_int(&source_vertex[0], 0),
                                   daal_string_to_int(&destination_vertex[0], 0));
        elist.push_back(edge);
    }

    file.close();
    return elist;
}

template <typename Graph>
void convert_to_csr_impl(const edge_list<vertex_type<Graph>> &edges, Graph &g) {
    if (edges.size() == 0) {
        throw invalid_argument("Empty edge list");
    }

    using vertex_t = typename Graph::vertex_type;
    using edge_t = typename Graph::edge_type;
    using vector_vertex_t = typename Graph::vertex_set;
    using atomic_t = typename daal::services::Atomic<vertex_t>;

    vertex_t max_id = edges[0].first;
    for (auto u : edges) {
        vertex_t edge_max = std::max(u.first, u.second);
        max_id = std::max(max_id, edge_max);
    }

    const vertex_t vertex_count = max_id + 1;

    auto layout = oneapi::dal::preview::detail::get_impl(g);
    auto &allocator = layout->_allocator;
    layout->_vertex_count = vertex_count;

    void *degrees_vec_void =
        (void *)allocator.allocate(vertex_count * (sizeof(atomic_t) / sizeof(char)));
    atomic_t *degrees_cv = new (degrees_vec_void) atomic_t[vertex_count];

    threader_for(edges.size(), edges.size(), [&](vertex_t u) {
        degrees_cv[edges[u].first].inc();
        degrees_cv[edges[u].second].inc();
    });

    void *rows_vec_void =
        (void *)allocator.allocate((vertex_count + 1) * (sizeof(atomic_t) / sizeof(char)));
    atomic_t *rows_vec_atomic = new (rows_vec_void) atomic_t[vertex_count + 1];

    vertex_t total_sum_degrees = 0;
    rows_vec_atomic[0].set(total_sum_degrees);
    for (vertex_t i = 0; i < vertex_count; ++i) {
        total_sum_degrees += degrees_cv[i].get();
        rows_vec_atomic[i + 1].set(total_sum_degrees);
    }
    allocator.deallocate((char *)degrees_vec_void,
                         vertex_count * (sizeof(atomic_t) / sizeof(char)));

    void *unfiltered_neighs_void = (void *)allocator.allocate(
        (rows_vec_atomic[vertex_count].get()) * (sizeof(vertex_t) / sizeof(char)));
    vertex_t *unfiltered_neighs =
        new (unfiltered_neighs_void) vertex_t[rows_vec_atomic[vertex_count].get()];

    void *unfiltered_offsets_void =
        (void *)allocator.allocate((vertex_count + 1) * (sizeof(edge_t) / sizeof(char)));
    edge_t *unfiltered_offsets = new (unfiltered_offsets_void) edge_t[vertex_count + 1];

    threader_for(vertex_count + 1, vertex_count + 1, [&](vertex_t n) {
        unfiltered_offsets[n] = rows_vec_atomic[n].get();
    });

    threader_for(edges.size(), edges.size(), [&](vertex_t u) {
        unfiltered_neighs[rows_vec_atomic[edges[u].first].inc() - 1] = edges[u].second;
        unfiltered_neighs[rows_vec_atomic[edges[u].second].inc() - 1] = edges[u].first;
    });
    allocator.deallocate((char *)rows_vec_void,
                         (vertex_count + 1) * (sizeof(atomic_t) / sizeof(char)));

    layout->_degrees = std::move(vector_vertex_t(vertex_count));
    auto degrees_data = layout->_degrees.data();

    //removing self-loops,  multiple edges from graph, and make neighbors in CSR sorted
    threader_for(vertex_count, vertex_count, [&](vertex_t u) {
        auto start_p = unfiltered_neighs + unfiltered_offsets[u];
        auto end_p = unfiltered_neighs + unfiltered_offsets[u + 1];

        std::sort(start_p, end_p);

        auto neighs_u_new_end = std::unique(start_p, end_p);
        neighs_u_new_end = std::remove(start_p, neighs_u_new_end, u);
        degrees_data[u] = (vertex_t)std::distance(start_p, neighs_u_new_end);
    });

    layout->_edge_offsets = std::move(vector_vertex_t(vertex_count + 1));
    auto edge_offsets_data = layout->_edge_offsets.data();

    total_sum_degrees = 0;
    edge_offsets_data[0] = total_sum_degrees;
    for (vertex_t i = 0; i < vertex_count; ++i) {
        total_sum_degrees += degrees_data[i];
        edge_offsets_data[i + 1] = total_sum_degrees;
    }
    layout->_edge_count = total_sum_degrees / 2;

    layout->_vertex_neighbors = std::move(vector_vertex_t(layout->_edge_offsets[vertex_count]));

    auto vert_neighs = layout->_vertex_neighbors.data();
    auto edge_offs = layout->_edge_offsets.data();
    threader_for(vertex_count, vertex_count, [&](vertex_t u) {
        auto u_neighs = vert_neighs + edge_offs[u];
        auto u_neighs_unf = unfiltered_neighs + unfiltered_offsets[u];
        for (vertex_t i = 0; i < degrees_data[u]; i++) {
            u_neighs[i] = u_neighs_unf[i];
        }
    });

    allocator.deallocate((char *)unfiltered_neighs_void,
                         (rows_vec_atomic[vertex_count].get()) * (sizeof(vertex_t) / sizeof(char)));
    allocator.deallocate((char *)unfiltered_offsets_void,
                         (vertex_count + 1) * (sizeof(edge_t) / sizeof(char)));
    return;
} // namespace oneapi::dal::preview::load_graph::detail

template <typename Descriptor, typename DataSource>
output_type<Descriptor> load_impl(const Descriptor &desc, const DataSource &data_source) {
    output_type<Descriptor> graph;
    convert_to_csr_impl(load_edge_list(data_source.get_filename()), graph);
    return graph;
}
} // namespace oneapi::dal::preview::load_graph::detail
