// Copyright (c) Facebook, Inc. and its affiliates.
// All rights reserved.
//
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree.

#include <ATen/ATen.h>
#include <functorch/csrc/DynamicLayer.h>
#include <functorch/csrc/BatchRulesHelper.h>

namespace at {
namespace functorch {

void check_randomness(RandomnessType randomness) {
  TORCH_CHECK(
    randomness != RandomnessType::Error,
    "vmap: called random operation while in randomness error mode. Please either use the "
    "'same' or 'different' randomness flags on vmap or perform the randomness operation out of vmap"
  );
}

template <typename F, F Func, typename... ExtraArgs>
Tensor random_batching_rule(IntArrayRef shape, ExtraArgs... extra_args) {
  c10::impl::ExcludeDispatchKeyGuard guard(kVmapModeKey);
  auto maybe_layer = maybeCurrentDynamicLayer();
  VmapDimVector shapeVec(1, maybe_layer->batchSize());
  shapeVec.reserve(shape.size() + 1);
  shapeVec.insert(shapeVec.end(), shape.begin(), shape.end());
  RandomnessType randomness = maybe_layer->randomness();
  check_randomness(randomness);
  if (randomness == RandomnessType::Different) {
    return makeBatched(Func(shapeVec, std::forward<ExtraArgs>(extra_args)...), 0, maybe_layer->layerId());
  } else {
    return Func(shape, std::forward<ExtraArgs>(extra_args)...);
  }
}

template <typename F, F Func, typename... ExtraArgs>
Tensor& random_inplace_batching_rule(Tensor& self, ExtraArgs... extra_args) {
  c10::impl::ExcludeDispatchKeyGuard guard(kVmapModeKey);
  auto maybe_layer = maybeCurrentDynamicLayer();
  const auto cur_level = maybe_layer->layerId();
  Tensor self_value;
  optional<int64_t> self_bdim;
  std::tie(self_value, self_bdim) = unwrapTensorAtLevel(self, cur_level);
  self_value = moveBatchDimToFront(self_value, self_bdim);
  RandomnessType randomness = maybe_layer->randomness();
  check_randomness(randomness);
  TORCH_CHECK(
    !(randomness == RandomnessType::Different && !self_bdim),
    "vmap: Cannot ask for different inplace randomness on an unbatched tensor. This will appear like same randomness. ",
    "If this is necessary for your usage, please file an issue with functorch.");
  if (randomness == RandomnessType::Same && self_bdim) {
    auto intermediate = empty(self.sizes(), self.options());
    Func(intermediate, std::forward<ExtraArgs>(extra_args)...);
    self.copy_(intermediate); // batching should make this just work out...
    return self;
  } else {
    Func(self_value, std::forward<ExtraArgs>(extra_args)...);
    return self;
  }
}

template <typename F, F Func, typename... ExtraArgs>
Tensor randperm_batching_rule(int64_t n, ExtraArgs... extra_args) {
  c10::impl::ExcludeDispatchKeyGuard guard(kVmapModeKey);
  auto maybe_layer = maybeCurrentDynamicLayer();
  auto const batch_size = maybe_layer->batchSize();
  RandomnessType randomness = maybe_layer->randomness();
  check_randomness(randomness);
  if (randomness == RandomnessType::Different) {
    std::vector<at::Tensor> stackedList(batch_size);
    stackedList.reserve(batch_size);
    for (int64_t idx = 0; idx < batch_size; ++idx) {
      // since this is done in a loop, need to pass by reference for generator to update
      stackedList[idx] = Func(n, extra_args...);
    }
    return makeBatched(at::stack(stackedList), 0, maybe_layer->layerId());
  } else {
    const auto res = Func(n, std::forward<ExtraArgs>(extra_args)...);
    return res;
  }
}

template <typename A, A a, typename C>
struct RandomBatchRuleHelper;

template <typename F, F Func, typename T1, typename... T>
struct RandomBatchRuleHelper<F, Func, typelist<T1, T...>> {
  static Tensor apply(IntArrayRef shape, T... extra_args) {
    return random_batching_rule<F, Func, T...>(shape, std::forward<T>(extra_args)...);
  }
};

template <typename F, F Func, typename... T>
Tensor rand_int_wrapper(IntArrayRef shape, int64_t high, T... extra_args) {
  return Func(high, shape, std::forward<T>(extra_args)...);
}

template <typename A, A a, typename C>
struct RandomInplaceBatchRuleHelper;

template <typename F, F Func, typename T1, typename... T>
struct RandomInplaceBatchRuleHelper<F, Func, typelist<T1, T...>> {
  static Tensor& apply(Tensor& self, T... extra_args) {
    return random_inplace_batching_rule<F, Func, T...>(self, std::forward<T>(extra_args)...);
  }
};

template <typename A, A a, typename C>
struct RandIntBatchRuleHelper;

template <typename F, F Func, typename T1, typename T2, typename... T>
struct RandIntBatchRuleHelper<F, Func, typelist<T1, T2, T...>> {
  static Tensor apply(int64_t high, IntArrayRef shape, T... extra_args) {
    return random_batching_rule<decltype(&rand_int_wrapper<F, Func, T...>),
                                &rand_int_wrapper<F, Func, T...>,
                                int64_t, T...>(shape, high, std::forward<T>(extra_args)...);
  }
};

template <typename F, F Func, typename... T>
Tensor rand_int_low_wrapper(IntArrayRef shape, int64_t high, int64_t low, T... extra_args) {
  return Func(high, low, shape, std::forward<T>(extra_args)...);
}

template <typename A, A a, typename C>
struct RandIntLowBatchRuleHelper;

template <typename F, F Func, typename T1, typename T2, typename T3, typename... T>
struct RandIntLowBatchRuleHelper<F, Func, typelist<T1, T2, T3, T...>> {
  static Tensor apply(int64_t high, int64_t low, IntArrayRef shape, T... extra_args) {
    return random_batching_rule<decltype(&rand_int_low_wrapper<F, Func, T...>),
                                &rand_int_low_wrapper<F, Func, T...>,
                                int64_t, int64_t, T...>(shape, high, low, std::forward<T>(extra_args)...);
  }
};

template <typename A, A a, typename C>
struct RandpermBatchRuleHelper;

template <typename F, F Func, typename T1, typename... T>
struct RandpermBatchRuleHelper<F, Func, typelist<T1, T...>> {
  static Tensor apply(int64_t n, T... extra_args) {
    return randperm_batching_rule<F, Func, T...>(n, std::forward<T>(extra_args)...);
  }
};

TORCH_LIBRARY_IMPL(aten, FuncTorchVmapMode, m) {
  #define RANDOM_BATCH_RULE(op) \
    m.impl(#op, SINGLE_ARG(\
      RandomBatchRuleHelper<decltype(&ATEN_FN(op)), &ATEN_FN(op), \
                            c10::guts::function_traits<decltype(ATEN_FN(op))>::parameter_types>::apply))

  #define RANDOM_BATCH_RULE2(op, overload) \
    m.impl(#op"."#overload, SINGLE_ARG(\
      RandomBatchRuleHelper<decltype(&ATEN_FN2(op, overload)), &ATEN_FN2(op, overload), \
                            c10::guts::function_traits<decltype(ATEN_FN2(op, overload))>::parameter_types>::apply))
  
  #define RANDOM_INPLACE_BATCH_RULE(op) \
    m.impl(#op, SINGLE_ARG(\
      RandomInplaceBatchRuleHelper<decltype(&ATEN_FN(op)), &ATEN_FN(op), \
                            c10::guts::function_traits<decltype(ATEN_FN(op))>::parameter_types>::apply))

  #define RANDOM_INPLACE_BATCH_RULE2(op, overload) \
    m.impl(#op"."#overload, SINGLE_ARG(\
      RandomInplaceBatchRuleHelper<decltype(&ATEN_FN2(op, overload)), &ATEN_FN2(op, overload), \
                            c10::guts::function_traits<decltype(ATEN_FN2(op, overload))>::parameter_types>::apply))

  #define RANDINT_BATCH_RULE(op) \
    m.impl(#op, SINGLE_ARG(\
      RandIntBatchRuleHelper<decltype(&ATEN_FN(op)), &ATEN_FN(op), \
                             c10::guts::function_traits<decltype(ATEN_FN(op))>::parameter_types>::apply))

  #define RANDINT_BATCH_RULE2(op, overload) \
    m.impl(#op"."#overload, SINGLE_ARG(\
      RandIntBatchRuleHelper<decltype(&ATEN_FN2(op, overload)), &ATEN_FN2(op, overload), \
                            c10::guts::function_traits<decltype(ATEN_FN2(op, overload))>::parameter_types>::apply))

  #define RANDINT_LOW_BATCH_RULE(op, overload) \
    m.impl(#op"."#overload, SINGLE_ARG(\
      RandIntLowBatchRuleHelper<decltype(&ATEN_FN2(op, overload)), &ATEN_FN2(op, overload), \
                                c10::guts::function_traits<decltype(ATEN_FN2(op, overload))>::parameter_types>::apply))
  #define RANDPERM_BATCH_RULE(op) \
    m.impl(#op, SINGLE_ARG(\
      RandpermBatchRuleHelper<decltype(&ATEN_FN(op)), &ATEN_FN(op), \
                            c10::guts::function_traits<decltype(ATEN_FN(op))>::parameter_types>::apply))

  #define RANDPERM_BATCH_RULE2(op, overload) \
    m.impl(#op"."#overload, SINGLE_ARG(\
      RandpermBatchRuleHelper<decltype(&ATEN_FN2(op, overload)), &ATEN_FN2(op, overload), \
                            c10::guts::function_traits<decltype(ATEN_FN2(op, overload))>::parameter_types>::apply))

  RANDOM_BATCH_RULE(randn);
  RANDOM_BATCH_RULE2(randn, generator);
  RANDOM_BATCH_RULE2(randn, generator_with_names);
  RANDOM_BATCH_RULE2(randn, names);

  RANDOM_BATCH_RULE(rand);
  RANDOM_BATCH_RULE2(rand, generator);
  RANDOM_BATCH_RULE2(rand, generator_with_names);
  RANDOM_BATCH_RULE2(rand, names);

  RANDOM_INPLACE_BATCH_RULE(random_);
  RANDOM_INPLACE_BATCH_RULE2(random_, from);
  RANDOM_INPLACE_BATCH_RULE2(random_, to);

  RANDOM_INPLACE_BATCH_RULE(normal_);

  RANDINT_BATCH_RULE(randint);
  RANDINT_BATCH_RULE2(randint, generator);
  RANDINT_LOW_BATCH_RULE(randint, low);
  RANDINT_LOW_BATCH_RULE(randint, low_generator);

  RANDPERM_BATCH_RULE(randperm);
  RANDPERM_BATCH_RULE2(randperm, generator);

  #undef RANDOM_BATCH_RULE
  #undef RANDOM_BATCH_RULE2
  #undef RANDOM_INPLACE_BATCH_RULE
  #undef RANDOM_INPLACE_BATCH_RULE2
  #undef RANDINT_BATCH_RULE
  #undef RANDINT_BATCH_RULE2
  #undef RANDINT_LOW_BATCH_RULE
  #undef RANDPERM_BATCH_RULE
  #undef RANDPERM_BATCH_RULE2
}
}} // namespace at::functorch