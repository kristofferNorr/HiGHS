/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/*                                                                       */
/*    This file is part of the HiGHS linear optimization suite           */
/*                                                                       */
/*    Written and engineered 2008-2021 at the University of Edinburgh    */
/*                                                                       */
/*    Available as open-source under the MIT License                     */
/*                                                                       */
/*    Authors: Julian Hall, Ivet Galabova, Qi Huangfu, Leona Gottwald    */
/*    and Michael Feldmeier                                              */
/*                                                                       */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/**@file util/HighsDataStack.h
 * @brief A stack of unstructured data stored as bytes
 */

#ifndef UTIL_HIGHS_DATA_STACK_H_
#define UTIL_HIGHS_DATA_STACK_H_

#include <cstring>
#include <type_traits>
#include <vector>

#if __GNUG__ && __GNUC__ < 5
#define IS_TRIVIALLY_COPYABLE(T) __has_trivial_copy(T)
#else
#define IS_TRIVIALLY_COPYABLE(T) std::is_trivially_copyable<T>::value
#endif

class HighsDataStack {
  std::vector<char> data;
  HighsInt position;

 public:
  void resetPosition() { position = data.size(); }

  template <typename T,
            typename std::enable_if<IS_TRIVIALLY_COPYABLE(T), int>::type = 0>
  void push(const T& r) {
    HighsInt dataSize = data.size();
    data.resize(dataSize + sizeof(T));
    std::memcpy(data.data() + dataSize, &r, sizeof(T));
  }

  template <typename T,
            typename std::enable_if<IS_TRIVIALLY_COPYABLE(T), int>::type = 0>
  void pop(T& r) {
    position -= sizeof(T);
    std::memcpy(&r, data.data() + position, sizeof(T));
  }

  template <typename T>
  void push(const std::vector<T>& r) {
    HighsInt offset = data.size();
    HighsInt numData = r.size();
    // store the data
    data.resize(offset + numData * sizeof(T) + sizeof(HighsInt));
    std::memcpy(data.data() + offset, r.data(), numData * sizeof(T));
    // store the vector size
    offset += numData * sizeof(T);
    std::memcpy(data.data() + offset, &numData, sizeof(HighsInt));
  }

  template <typename T>
  void pop(std::vector<T>& r) {
    // pop the vector size
    position -= sizeof(HighsInt);
    HighsInt numData;
    std::memcpy(&numData, &data[position], sizeof(HighsInt));
    // pop the data
    position -= numData * sizeof(T);
    r.resize(numData);
    std::memcpy(r.data(), data.data() + position, numData * sizeof(T));
  }
};

#endif