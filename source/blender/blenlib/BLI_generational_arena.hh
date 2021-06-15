/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#pragma once

/** \file
 * \ingroup bli
 *
 * A `blender::generation_arena<T>` is a dynamically growing
 * contiguous array for values of type T. It is designed to have
 * a similar api as `blender::Vector<T>` but with generational
 * indices. There are benefits to generational arenas.
 *
 * **How it works**
 * The `Arena` has a `Vector` of `Entry`(s), an optional location of
 * next `EntryNoExist` position in the `Vector`, current generation and the
 * length (note: cannot use `Vector`'s length since any element in the
 * `Arena` can be deleted but this doesn't affect the length of the
 * vector).
 *
 * Insertion involves finding a `EntryNoExist` position, if it exists,
 * update the `Arena` with the next `EntryNoExist` position with the
 * `next_free` stored in the position that is now filled. At this
 * position, set to `EntryExist` and let the `generation` be current
 * generation value in the `Arena` and value as the value supplied by
 * the user.
 *
 * Deletion involves updating setting that location to `EntryNoExist`
 * with the `next_free` set as the `Arena`'s `next_free` and updating
 * the `Arena`'s `next_free` to the location that is to be
 * deleted. The generation should also be incremented as well as the
 * length.
 *
 * When user requests for a value using `Index`, the `generation` is
 * verified to match the generation at that `index`, if it doesn't
 * match, the value at that position was deleted and then some other
 * value was inserted which means the requested value doesn't exist at
 * that location.
 */
/* TODO(ish): need to complete documentation */

#include <functional>
#include <limits>
#include <optional>
#include <tuple>
#include <variant>

#include "BLI_vector.hh"

namespace blender::generational_arena {

namespace extra {
template<typename... Ts> struct overloaded : Ts... {
  using Ts::operator()...;
};
template<typename... Ts> overloaded(Ts...) -> overloaded<Ts...>;
} /* namespace extra */

class Index {
  using usize = uint64_t;

  usize index;
  usize generation;

 public:
  Index(usize index, usize generation)
  {
    this->index = index;
    this->generation = generation;
  }

  std::tuple<usize, usize> get_raw() const
  {
    return std::make_tuple(this->index, this->generation);
  }
};

template<
    /**
     * Type of the values stored in this vector. It has to be movable.
     */
    typename T,
    /**
     * The number of values that can be stored in this vector, without doing a heap allocation.
     * Sometimes it makes sense to increase this value a lot. The memory in the inline buffer is
     * not initialized when it is not needed.
     *
     * When T is large, the small buffer optimization is disabled by default to avoid large
     * unexpected allocations on the stack. It can still be enabled explicitly though.
     */
    int64_t InlineBufferCapacity = default_inline_buffer_capacity(sizeof(T)),
    /**
     * The allocator used by this vector. Should rarely be changed, except when you don't want that
     * MEM_* is used internally.
     */
    typename Allocator = GuardedAllocator>
class Arena {
  struct EntryNoExist;
  struct EntryExist;
  /* using declarations */
  using usize = uint64_t;
  using isize = int64_t;
  using Entry = std::variant<EntryNoExist, EntryExist>;

  /* static data members */
  /* non-static data members */
  struct EntryNoExist {
    std::optional<usize> next_free;

    EntryNoExist()
    {
    }

    EntryNoExist(usize next_free)
    {
      this->next_free = next_free;
    }

    EntryNoExist(std::optional<usize> next_free)
    {
      this->next_free = next_free;
    }
  };
  struct EntryExist {
    T value;
    usize generation;

    EntryExist(T value, usize generation)
    {
      this->value = value;
      this->generation = generation;
    }
  };

  blender::Vector<Entry> data;
  std::optional<usize> next_free_head;
  usize generation;
  usize length;

 public:
  /* default constructor */
  Arena() = default;
  /* other constructors */
  Arena(const usize size) : Arena()
  {
    this->reserve(size);
  }
  /* copy constructor */
  /* move constructor */

  /* destructor */

  /* copy assignment operator */
  /* move assignment operator */
  /* other operator overloads */

  /* all public static methods */
  /* all public non-static methods */
  void reserve(const usize new_cap)
  {
    /* Must only increase capacity */
    if (new_cap < this->data.size()) {
      return;
    }

    this->data.reserve(new_cap);
    /* next_free_head is set to start of extended list
     *
     * in the extended elements, next_free is set to the next element
     *
     * last element in the extended elements's next_free is the old
     * next_free_head */
    auto const old_next_free_head = this->next_free_head;
    auto const start = this->data.size();
    for (auto i = start; i < new_cap - 1; i++) {
      this->data.append(EntryNoExist(i + 1));
    }
    this->data.append(EntryNoExist(old_next_free_head));
    this->next_free_head = start;
  }

  /* TODO(ish): add optimization by moving `value`, can be done by
   * returning value if `try_insert()` fails */
  std::optional<Index> try_insert(T value)
  {
    if (this->next_free_head) {
      auto loc = this->next_free_head;
      std::visit(extra::overloaded{[this, value](EntryNoExist &data) {
                                     this->next_free_head = data.next_free;
                                     data = EntryExist(value, this->generation);
                                   },
                                   [](EntryExist &data) {
                                     /* The linked list created to
                                      * know where to insert next is
                                      * corrupted.
                                      * `this->next_free_head` is corrupted */
                                     BLI_assert_unreachable();
                                   }},
                 this->data[loc]);
      this->length += 1;
      return Index(loc, this->generation);
    }
    return std::nullopt;
  }

  Index insert(T value)
  {
    if (auto index = this->try_insert(value)) {
      return index;
    }
    else {
      /* couldn't insert the value within reserved memory space  */
      /* TODO(ish): might be possible that `this->data.size()` is 0,
       * needs a special case for that */
      this->reserve(this->data.size() * 2);
      if (auto index = this->try_insert(value)) {
        return index;
      }
      else {
        /* now that more memory has been reserved, it shouldn't fail */
        BLI_assert_unreachable();
      }
    }
  }

  std::optional<std::reference_wrapper<const T>> get(Index index) const
  {
    /* if index exceeds size of the container, return std::nullopt */
    if (index.index >= this->data.size()) {
      return std::nullopt;
    }

    if (index.generation != this->data[index.index]) {
      return std::nullopt;
    }

    return std::cref(this->data[index.index]);
  }

  std::optional<std::reference_wrapper<T>> get(Index index)
  {
    /* if index exceeds size of the container, return std::nullopt */
    if (index.index >= this->data.size()) {
      return std::nullopt;
    }

    if (index.generation != this->data[index.index]) {
      return std::nullopt;
    }

    return std::ref(this->data[index.index]);
  }

  std::optional<std::reference_wrapper<const T>> get_no_gen(usize index) const
  {
    /* if index exceeds size of the container, return std::nullopt */
    if (index >= this->data.size()) {
      return std::nullopt;
    }

    return std::cref(this->data[index]);
  }

  std::optional<std::reference_wrapper<T>> get_no_gen(usize index)
  {
    /* if index exceeds size of the container, return std::nullopt */
    if (index >= this->data.size()) {
      return std::nullopt;
    }

    return std::ref(this->data[index]);
  }

  std::optional<Index> get_no_gen_index(usize index) const
  {
    /* if index exceeds size of the container, return std::nullopt */
    if (index >= this->data.size()) {
      return std::nullopt;
    }

    std::optional<Index> res;
    std::visit(extra::overloaded{
                   [&res](EntryNoExist &entry) { res = std::nullopt; },
                   [&res, index](EntryExist &entry) { res = Index(index, entry.generation); }},
               this->data[index]);

    return res;
  }

 protected:
  /* all protected static methods */
  /* all protected non-static methods */

 private:
  /* all private static methods */
  /* all private non-static methods */
};

} /* namespace blender::generational_arena */
