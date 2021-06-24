#define FORCE_IMPORT_ARRAY

#include <pybind11/pybind11.h>
#include <pybind11/numpy.h>
#include <pybind11/functional.h>

#include <xtensor-python/pytensor.hpp>

#include "solver.h"

namespace py = pybind11;


class Alignment {
public:
	typedef int16_t Index;

private:
	xt::pytensor<Index, 1> m_s_to_t;
	xt::pytensor<Index, 1> m_t_to_s;
	float m_score;

public:
	inline void resize(const size_t len_s, const size_t len_t) {
		m_s_to_t.resize({static_cast<ssize_t>(len_s)});
		m_s_to_t.fill(-1);

		m_t_to_s.resize({static_cast<ssize_t>(len_t)});
		m_t_to_s.fill(-1);
	}

	inline void add_edge(const size_t u, const size_t v) {
		m_s_to_t[u] = v;
		m_t_to_s[v] = u;
	}

	inline void set_score(const float p_score) {
		m_score = p_score;
	}

	inline float score() const {
		return m_score;
	}

	inline const xt::pytensor<Index, 1> &s_to_t() const {
		return m_s_to_t;
	}

	inline const xt::pytensor<Index, 1> &t_to_s() const {
		return m_t_to_s;
	}
};

typedef std::shared_ptr<Alignment> AlignmentRef;

class Solution {
public:
	typedef float Value;
	typedef Alignment::Index Index;

private:
	const pyalign::SolutionRef<Index, Value> m_solution;
	AlignmentRef m_alignment;

public:
	inline Solution(
		const pyalign::SolutionRef<Index, Value> p_solution,
		const AlignmentRef &p_alignment) :

		m_solution(p_solution),
		m_alignment(p_alignment) {
	}

	xt::pytensor<Value, 3> values() const {
		return m_solution->values();
	}

	xt::pytensor<Index, 4> traceback() const {
		return m_solution->traceback();
	}

	xt::pytensor<Index, 2> path() const {
		return m_solution->path();
	}

	auto score() const {
		return m_solution->score();
	}

	const auto &complexity() const {
		return m_solution->complexity();
	}

	AlignmentRef alignment() const {
		return m_alignment;
	}
};

typedef std::shared_ptr<Solution> SolutionRef;

typedef pyalign::Complexity Complexity;
typedef pyalign::ComplexityRef ComplexityRef;

class Solver {
public:
	virtual inline ~Solver() {
	}

	virtual const py::dict &options() const = 0;

	virtual float solve_for_score(
		const xt::pytensor<float, 2> &p_similarity) const = 0;

	virtual AlignmentRef solve_for_alignment(
		const xt::pytensor<float, 2> &p_similarity) const = 0;

	virtual SolutionRef solve_for_solution(
		const xt::pytensor<float, 2> &p_similarity) const = 0;
};

typedef std::shared_ptr<Solver> SolverRef;


template<typename S>
class SolverImpl : public Solver {
private:
	const py::dict m_options;
	S m_solver;

public:
	template<typename... Args>
	inline SolverImpl(const py::dict &p_options, const Args&... args) :
		m_options(p_options),
		m_solver(args...) {
	}

	virtual const py::dict &options() const override {
		return m_options;
	}

	virtual float solve_for_score(
		const xt::pytensor<float, 2> &p_similarity) const override {

		const auto len_s = p_similarity.shape(0);
		const auto len_t = p_similarity.shape(1);

		m_solver.template solve<pyalign::ComputationGoal::score>(
			p_similarity, len_s, len_t);
		return m_solver.score(len_s, len_t);
	}

	virtual AlignmentRef solve_for_alignment(
		const xt::pytensor<float, 2> &p_similarity) const override {

		const auto len_s = p_similarity.shape(0);
		const auto len_t = p_similarity.shape(1);

		m_solver.template solve<pyalign::ComputationGoal::alignment>(
			p_similarity, len_s, len_t);

		const auto alignment = std::make_shared<Alignment>();
		const float score = m_solver.alignment(
			len_s, len_t, *alignment.get());
		alignment->set_score(score);
		return alignment;
	}

	virtual SolutionRef solve_for_solution(
		const xt::pytensor<float, 2> &p_similarity) const override {

		const auto len_s = p_similarity.shape(0);
		const auto len_t = p_similarity.shape(1);

		m_solver.template solve<pyalign::ComputationGoal::alignment>(
			p_similarity, len_s, len_t);

		const auto alignment = std::make_shared<Alignment>();
		return std::make_shared<Solution>(
			m_solver.solution(len_s, len_t, *alignment.get()),
			alignment);
	}
};

inline xt::pytensor<float, 1> zero_gap_tensor(const size_t p_len) {
	xt::pytensor<float, 1> w;
	w.resize({static_cast<ssize_t>(p_len)});
	w.fill(0);
	return w;
}

inline pyalign::GapTensorFactory<float> to_gap_tensor_factory(const py::object &p_gap) {
	if (p_gap.is_none()) {
		return zero_gap_tensor;
	} else {
		return p_gap.attr("costs").cast<pyalign::GapTensorFactory<float>>();
	}
}

struct GapCostSpecialCases {
	inline GapCostSpecialCases(const py::object &p_gap) {
		if (p_gap.is_none()) {
			linear = 0.0f;
		} else {
	        const py::dict cost = p_gap.attr("to_special_case")().cast<py::dict>();
	        if (cost.contains("linear")) {
	            linear = cost["linear"].cast<float>();
	        }
	    }
	}

	std::optional<float> linear;
};

template<template<typename, typename, typename> class InternalSolver, typename Locality, typename... Args>
SolverRef create_alignment_solver_instance_for_direction(
	const py::dict &p_options,
	const Args&... args) {

	const std::string direction = p_options["direction"].cast<std::string>();

	if (direction == "maximize") {
		return std::make_shared<SolverImpl<InternalSolver<
			pyalign::Direction::maximize, Locality, Alignment::Index>>>(
				p_options, args...);
	} else if (direction == "minimize") {
		return std::make_shared<SolverImpl<InternalSolver<
			pyalign::Direction::minimize, Locality, Alignment::Index>>>(
				p_options, args...);
	} else {
		throw std::invalid_argument(direction);
	}
}

template<typename Locality>
SolverRef create_alignment_solver_instance(
	const py::dict &p_options,
	const Locality &p_locality,
	const py::object &p_gap_s,
	const py::object &p_gap_t,
	const size_t p_max_len_s,
	const size_t p_max_len_t) {

	const GapCostSpecialCases x_gap_s(p_gap_s);
	const GapCostSpecialCases x_gap_t(p_gap_t);

	if (x_gap_s.linear.has_value() && x_gap_t.linear.has_value()) {
		return create_alignment_solver_instance_for_direction<pyalign::LinearGapCostSolver, Locality>(
			p_options,
			p_locality,
			x_gap_s.linear.value(),
			x_gap_t.linear.value(),
			p_max_len_s,
			p_max_len_t
		);
	} else {
		return create_alignment_solver_instance_for_direction<pyalign::GeneralGapCostSolver, Locality>(
			p_options,
			p_locality,
			to_gap_tensor_factory(p_gap_s),
			to_gap_tensor_factory(p_gap_t),
			p_max_len_s,
			p_max_len_t
		);
	}
}

SolverRef create_alignment_solver(
	const size_t p_max_len_s,
	const size_t p_max_len_t,
	const py::dict &p_options) {

	const std::string locality_name = p_options.contains("locality") ?
		p_options["locality"].cast<std::string>() : "local";

	const py::object gap_cost = p_options.contains("gap_cost") ?
		p_options["gap_cost"] : py::none().cast<py::object>();

	py::object gap_s = py::none();
	py::object gap_t = py::none();

	if (py::isinstance<py::dict>(gap_cost)) {
		const py::dict gap_cost_dict = gap_cost.cast<py::dict>();

		if (gap_cost_dict.contains("s")) {
			gap_s = gap_cost_dict["s"];
		}
		if (gap_cost_dict.contains("t")) {
			gap_t = gap_cost_dict["t"];
		}
	} else {
		gap_s = gap_cost;
		gap_t = gap_cost;
	}

	if (locality_name == "local") {
		const float zero = p_options.contains("zero") ?
			p_options["zero"].cast<float>() : 0.0f;

		const auto locality = pyalign::Local<Alignment::Index, float>(zero);

		return create_alignment_solver_instance(
			p_options,
			locality,
			gap_s,
			gap_t,
			p_max_len_s,
			p_max_len_t);

	} else if (locality_name == "global") {

		const auto locality = pyalign::Global<Alignment::Index, float>();

		return create_alignment_solver_instance(
			p_options,
			locality,
			gap_s,
			gap_t,
			p_max_len_s,
			p_max_len_t);

	} else if (locality_name == "semiglobal") {

		const auto locality = pyalign::Semiglobal<Alignment::Index, float>();

		return create_alignment_solver_instance(
			p_options,
			locality,
			gap_s,
			gap_t,
			p_max_len_s,
			p_max_len_t);

	} else {

		throw std::invalid_argument(locality_name);
	}
}

SolverRef create_dtw_solver(
	const size_t p_max_len_s,
	const size_t p_max_len_t,
	const py::dict &p_options) {

	const std::string direction = p_options["direction"].cast<std::string>();

	if (direction == "maximize") {
		return std::make_shared<SolverImpl<pyalign::DynamicTimeSolver<
			pyalign::Direction::maximize, Alignment::Index, float>>>(
			p_options,
			p_max_len_s,
			p_max_len_t);
	} else if (direction == "minimize") {
		return std::make_shared<SolverImpl<pyalign::DynamicTimeSolver<
			pyalign::Direction::minimize, Alignment::Index, float>>>(
			p_options,
			p_max_len_s,
			p_max_len_t);
	} else {
		throw std::invalid_argument(direction);
	}
}

SolverRef create_solver(
	const size_t p_max_len_s,
	const size_t p_max_len_t,
	const py::dict &p_options) {

	const std::string solver = p_options["solver"].cast<py::str>();
	if (solver == "alignment") {
		return create_alignment_solver(
			p_max_len_s, p_max_len_t, p_options);
	} else if (solver == "dtw") {
		return create_dtw_solver(
			p_max_len_s, p_max_len_t, p_options);
	} else {
		throw std::invalid_argument(solver);
	}
}

PYBIND11_MODULE(algorithm, m) {
	xt::import_numpy();

	m.def("create_solver", &create_solver);

	py::class_<Solver, SolverRef> solver(m, "Solver");
	solver.def_property_readonly("options", &Solver::options);
	solver.def("solve_for_score", &Solver::solve_for_score);
	solver.def("solve_for_alignment", &Solver::solve_for_alignment);
	solver.def("solve_for_solution", &Solver::solve_for_solution);

	py::class_<Alignment, AlignmentRef> alignment(m, "Alignment");
	alignment.def_property_readonly("score", &Alignment::score);
	alignment.def_property_readonly("s_to_t", &Alignment::s_to_t);
	alignment.def_property_readonly("t_to_s", &Alignment::t_to_s);

	py::class_<Solution, SolutionRef> solution(m, "Solution");
	solution.def_property_readonly("values", &Solution::values);
	solution.def_property_readonly("traceback", &Solution::traceback);
	solution.def_property_readonly("path", &Solution::path);
	solution.def_property_readonly("score", &Solution::score);
	solution.def_property_readonly("alignment", &Solution::alignment);

	py::class_<Complexity, ComplexityRef> complexity(m, "Complexity");
	complexity.def_property_readonly("runtime", &Complexity::runtime);
	complexity.def_property_readonly("memory", &Complexity::memory);
}
