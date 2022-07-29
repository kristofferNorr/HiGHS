/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/*                                                                       */
/*    This file is part of the HiGHS linear optimization suite           */
/*                                                                       */
/*    Written and engineered 2008-2022 at the University of Edinburgh    */
/*                                                                       */
/*    Available as open-source under the MIT License                     */
/*                                                                       */
/*    Authors: Julian Hall, Ivet Galabova, Leona Gottwald and Michael    */
/*    Feldmeier                                                          */
/*                                                                       */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
#ifndef HIGHS_UTIL_HASH_TREE_H_
#define HIGHS_UTIL_HASH_TREE_H_

#include "util/HighsHash.h"

using std::memcpy;
using std::memmove;

template <typename K, typename V = void>
class HighsHashTree {
  using Entry = HighsHashTableEntry<K, V>;
  using ValueType = typename std::remove_reference<
      decltype(reinterpret_cast<Entry*>(0x1)->value())>::type;

  enum Type {
    kEmpty = 0,
    kListLeaf = 1,
    kInnerLeafSizeClass1 = 2,
    kInnerLeafSizeClass2 = 3,
    kInnerLeafSizeClass3 = 4,
    kInnerLeafSizeClass4 = 5,
    kBranchNode = 6,
  };

  enum Constants {
    kBitsPerLevel = 6,
    kBranchFactor = 1 << kBitsPerLevel,
    kMaxDepth = (64 + (kBitsPerLevel - 1)) / kBitsPerLevel,
    kMinLeafSize = 6,
    kLeafBurstThreshold = 30,
  };

  static uint8_t get_hash_chunk(uint64_t hash, int pos) {
    return (hash >> (pos * kBitsPerLevel)) & (kBranchFactor - 1);
  }

  static void set_hash_chunk(uint64_t& hash, uint64_t chunk, int chunkPos) {
    const int shiftAmount = chunkPos * kBitsPerLevel;
    chunk ^= (hash >> shiftAmount) & (kBranchFactor - 1);
    hash ^= chunk << shiftAmount;
  }

  static uint64_t get_first_n_hash_chunks(uint64_t hash, int n) {
    return hash & ((uint64_t{1} << (n * kBitsPerLevel)) - 1);
  }

  struct Occupation {
    uint64_t occupation;

    Occupation(uint64_t occupation) : occupation(occupation) {}
    operator uint64_t() const { return occupation; }

    void set(uint8_t pos) { occupation |= uint64_t{1} << (pos); }

    void flip(uint8_t pos) { occupation ^= uint64_t{1} << (pos); }

    bool test(uint8_t pos) const { return occupation & (uint64_t{1} << pos); }

    int num_set_until(uint8_t pos) const {
      return HighsHashHelpers::popcnt(occupation >> pos);
    }

    int num_set_after(uint8_t pos) const {
      return HighsHashHelpers::popcnt(occupation << (63 - (pos)));
    }

    int num_set() const { return HighsHashHelpers::popcnt(occupation); }
  };

  template <int kSizeClass>
  struct InnerLeaf {
    static constexpr int capacity() {
      return kMinLeafSize +
             (kSizeClass - 1) * (kLeafBurstThreshold - kMinLeafSize) / 3;
    }
    // the leaf stores entries the same way as inner nodes
    // but handles collisions on the occupation flag like a
    // linear probing hash table.
    // Since the occupation flag has 64 bits and we only use
    // 15 collisions should be rare and most often we won't need
    // to do a linear scan and key comparisons at all
    Occupation occupation;
    int size;

    HighsHashTableEntry<K, V> entries[capacity()];

    InnerLeaf() : occupation(0), size(0) {}

    int get_num_entries() const { return size; }

    bool insert_entry(uint64_t hash, int hashPos,
                      HighsHashTableEntry<K, V>& entry) {
      assert(size < capacity());
      uint8_t hashChunk = get_hash_chunk(hash, hashPos);

      if (occupation.test(hashChunk)) {
        // check if item already contained first
        int i = 0;
        while (true) {
          if (entry.key() <= entries[i].key()) break;
          if (++i == size) {
            entries[i] = std::move(entry);
            ++size;
            return true;
          }
        }

        if (entry.key() == entries[i].key()) return false;

        std::move_backward(&entries[i], &entries[size], &entries[size + 1]);
        entries[i] = std::move(entry);
        ++size;
      } else {
        occupation.set(hashChunk);

        int i = 0;
        if (size) {
          do {
            if (entry.key() < entries[i].key()) break;
          } while (++i < size);

          std::move_backward(&entries[i], &entries[size], &entries[size + 1]);
        }
        entries[i] = std::move(entry);
        ++size;
      }

      return true;
    }

    const ValueType* find_entry(uint64_t hash, int hashPos,
                                const K& key) const {
      uint8_t hashChunk = get_hash_chunk(hash, hashPos);
      if (!occupation.test(hashChunk)) return nullptr;

      int i = 0;
      while (true) {
        if (key <= entries[i].key()) break;
        if (++i == size) return nullptr;
      }

      if (key == entries[i].key()) return &entries[i].value();

      return nullptr;
    }

    bool erase_entry(uint64_t hash, int hashPos, const K& key) {
      uint8_t hashChunk = get_hash_chunk(hash, hashPos);
      if (!occupation.test(hashChunk)) return false;

      int i = 0;
      int foundElement = 0;
      int foundHashChunk = 0;
      do {
        uint8_t chunk =
            get_hash_chunk(HighsHashHelpers::hash(entries[i].key()), hashPos);
        foundHashChunk += chunk == hashChunk;
        if (foundHashChunk) {
          if (foundElement) {
            entries[i - 1] = std::move(entries[i]);
          } else
            foundElement = key == entries[i].key();
        }
      } while (++i < size);

      // if there are other elements remaining with the same hash chunk we need
      // to keep the occupation flag set
      if (foundElement) {
        if (foundHashChunk == 1) occupation.flip(hashChunk);

        // reduce size by 1 if the element has been erased
        size -= 1;
      }

      return foundElement;
    }
  };

  struct ListNode {
    ListNode* next;
    HighsHashTableEntry<K, V> entry;
    ListNode(HighsHashTableEntry<K, V>&& entry)
        : next(nullptr), entry(std::move(entry)) {}
  };
  struct ListLeaf {
    ListNode first;
    int count;

    ListLeaf(HighsHashTableEntry<K, V>&& entry)
        : first(std::move(entry)), count(1) {}
  };

  struct BranchNode;

  struct NodePtr {
    uintptr_t ptrAndType;

    NodePtr() : ptrAndType(kEmpty) {}
    NodePtr(std::nullptr_t) : ptrAndType(kEmpty) {}
    NodePtr(ListLeaf* ptr)
        : ptrAndType(reinterpret_cast<uintptr_t>(ptr) | kListLeaf) {}
    NodePtr(InnerLeaf<1>* ptr)
        : ptrAndType(reinterpret_cast<uintptr_t>(ptr) | kInnerLeafSizeClass1) {}
    NodePtr(InnerLeaf<2>* ptr)
        : ptrAndType(reinterpret_cast<uintptr_t>(ptr) | kInnerLeafSizeClass2) {}
    NodePtr(InnerLeaf<3>* ptr)
        : ptrAndType(reinterpret_cast<uintptr_t>(ptr) | kInnerLeafSizeClass3) {}
    NodePtr(InnerLeaf<4>* ptr)
        : ptrAndType(reinterpret_cast<uintptr_t>(ptr) | kInnerLeafSizeClass4) {}
    NodePtr(BranchNode* ptr)
        : ptrAndType(reinterpret_cast<uintptr_t>(ptr) | kBranchNode) {
      assert(ptr != nullptr);
    }

    Type getType() const { return Type(ptrAndType & 7u); }

    int numEntriesEstimate() const {
      switch (getType()) {
        case kEmpty:
          return 0;
        case kListLeaf:
          return 1;
        case kInnerLeafSizeClass1:
          return InnerLeaf<1>::capacity();
        case kInnerLeafSizeClass2:
          return InnerLeaf<2>::capacity();
        case kInnerLeafSizeClass3:
          return InnerLeaf<3>::capacity();
        case kInnerLeafSizeClass4:
          return InnerLeaf<4>::capacity();
        case kBranchNode:
          // something large should be returned so that the number of entries
          // is estimated above the threshold to merge when the parent checks
          // its children after deletion
          return kBranchFactor;
      }
    }

    int numEntries() const {
      switch (getType()) {
        case kEmpty:
          return 0;
        case kListLeaf:
          return getListLeaf()->count;
        case kInnerLeafSizeClass1:
          return getInnerLeafSizeClass1()->size;
        case kInnerLeafSizeClass2:
          return getInnerLeafSizeClass2()->size;
        case kInnerLeafSizeClass3:
          return getInnerLeafSizeClass3()->size;
        case kInnerLeafSizeClass4:
          return getInnerLeafSizeClass4()->size;
        case kBranchNode:
          // something large should be returned so that the number of entries
          // is estimated above the threshold to merge when the parent checks
          // its children after deletion
          return kBranchFactor;
      }
    }

    ListLeaf* getListLeaf() const {
      assert(getType() == kListLeaf);
      return reinterpret_cast<ListLeaf*>(ptrAndType & ~uintptr_t{7});
    }

    InnerLeaf<1>* getInnerLeafSizeClass1() const {
      assert(getType() == kInnerLeafSizeClass1);
      return reinterpret_cast<InnerLeaf<1>*>(ptrAndType & ~uintptr_t{7});
    }
    InnerLeaf<2>* getInnerLeafSizeClass2() const {
      assert(getType() == kInnerLeafSizeClass2);
      return reinterpret_cast<InnerLeaf<2>*>(ptrAndType & ~uintptr_t{7});
    }

    InnerLeaf<3>* getInnerLeafSizeClass3() const {
      assert(getType() == kInnerLeafSizeClass3);
      return reinterpret_cast<InnerLeaf<3>*>(ptrAndType & ~uintptr_t{7});
    }

    InnerLeaf<4>* getInnerLeafSizeClass4() const {
      assert(getType() == kInnerLeafSizeClass4);
      return reinterpret_cast<InnerLeaf<4>*>(ptrAndType & ~uintptr_t{7});
    }

    BranchNode* getBranchNode() const {
      assert(getType() == kBranchNode);
      return reinterpret_cast<BranchNode*>(ptrAndType & ~uintptr_t{7});
    }
  };

  struct BranchNode {
    Occupation occupation;
    NodePtr child[1];
  };

  // allocate branch nodes in multiples of 64 bytes to reduce allocator stress
  // with different sizes and reduce reallocations of nodes
  static constexpr size_t getBranchNodeSize(int numChilds) {
    return (sizeof(BranchNode) + size_t(numChilds - 1) * sizeof(NodePtr) + 63) &
           ~63;
  };

  static BranchNode* createBranchingNode(int numChilds) {
    BranchNode* branch =
        (BranchNode*)::operator new(getBranchNodeSize(numChilds));
    branch->occupation = 0;
    return branch;
  }

  static void destroyBranchingNode(void* innerNode) {
    ::operator delete(innerNode);
  }

  static BranchNode* addChildToBranchNode(BranchNode* branch, uint8_t hashValue,
                                          int location) {
    int rightChilds = branch->occupation.num_set_after(hashValue);
    assert(rightChilds + location == branch->occupation.num_set());

    size_t newSize = getBranchNodeSize(location + rightChilds + 1);
    size_t rightSize = rightChilds * sizeof(NodePtr);

    if (newSize == getBranchNodeSize(location + rightChilds)) {
      memmove(&branch->child[location + 1], &branch->child[location],
              rightSize);

      return branch;
    }

    BranchNode* newBranch = (BranchNode*)::operator new(newSize);
    // sizeof(Branch) already contains the size for 1 pointer. So we just
    // need to add the left and right sizes up for the number of
    // additional pointers
    size_t leftSize = sizeof(BranchNode) + (location - 1) * sizeof(NodePtr);

    memcpy(newBranch, branch, leftSize);
    memcpy(&newBranch->child[location + 1], &branch->child[location],
           rightSize);

    destroyBranchingNode(branch);

    return newBranch;
  }

  template <int SizeClass1, int SizeClass2>
  static void mergeIntoLeaf(InnerLeaf<SizeClass1>* leaf,
                            InnerLeaf<SizeClass2>* mergeLeaf, int hashPos) {
    for (int i = 0; i < mergeLeaf->size; ++i)
      leaf->insert_entry(HighsHashHelpers::hash(mergeLeaf->entries[i].key()),
                         hashPos, mergeLeaf->entries[i]);
  }

  template <int SizeClass>
  static void mergeIntoLeaf(InnerLeaf<SizeClass>* leaf, int hashPos,
                            NodePtr mergeNode) {
    switch (mergeNode.getType()) {
      case kListLeaf: {
        ListLeaf* mergeLeaf = mergeNode.getListLeaf();
        leaf->insert_entry(HighsHashHelpers::hash(mergeLeaf->first.entry.key()),
                           hashPos, mergeLeaf->first.entry);
        ListNode* iter = mergeLeaf->first.next;
        while (iter != nullptr) {
          ListNode* next = iter->next;
          leaf->insert_entry(HighsHashHelpers::hash(iter->entry.key()), hashPos,
                             iter->entry);
          delete iter;
          iter = next;
        }
        break;
      }
      case kInnerLeafSizeClass1:
        mergeIntoLeaf(leaf, mergeNode.getInnerLeafSizeClass1(), hashPos);
        delete mergeNode.getInnerLeafSizeClass1();
        break;
      case kInnerLeafSizeClass2:
        mergeIntoLeaf(leaf, mergeNode.getInnerLeafSizeClass2(), hashPos);
        delete mergeNode.getInnerLeafSizeClass2();
        break;
      case kInnerLeafSizeClass3:
        mergeIntoLeaf(leaf, mergeNode.getInnerLeafSizeClass3(), hashPos);
        delete mergeNode.getInnerLeafSizeClass3();
        break;
      case kInnerLeafSizeClass4:
        mergeIntoLeaf(leaf, mergeNode.getInnerLeafSizeClass4(), hashPos);
        delete mergeNode.getInnerLeafSizeClass4();
    }
  }

  template <int SizeClass1, int SizeClass2>
  static HighsHashTableEntry<K, V>* findCommonInLeaf(
      InnerLeaf<SizeClass1>* leaf1, InnerLeaf<SizeClass2>* leaf2) {
    if ((leaf1->occupation & leaf2->occupation) == 0) return nullptr;

    if (leaf1->entries[leaf1->size - 1].key() < leaf2->entries[0].key() ||
        leaf2->entries[leaf2->size - 1].key() < leaf1->entries[0].key())
      return nullptr;

    int i = 0;
    int j = 0;

    do {
      if (leaf1->entries[i].key() < leaf2->entries[j].key())
        ++i;
      else if (leaf2->entries[j].key() < leaf1->entries[i].key())
        ++j;
      else
        return &leaf1->entries[i];
    } while (i < leaf1->size && j < leaf2->size);

    return nullptr;
  }

  template <int SizeClass>
  static HighsHashTableEntry<K, V>* findCommonInLeaf(InnerLeaf<SizeClass>* leaf,
                                                     NodePtr n2, int hashPos) {
    switch (n2.getType()) {
      case kInnerLeafSizeClass1:
        return findCommonInLeaf(leaf, n2.getInnerLeafSizeClass1());
      case kInnerLeafSizeClass2:
        return findCommonInLeaf(leaf, n2.getInnerLeafSizeClass2());
      case kInnerLeafSizeClass3:
        return findCommonInLeaf(leaf, n2.getInnerLeafSizeClass3());
      case kInnerLeafSizeClass4:
        return findCommonInLeaf(leaf, n2.getInnerLeafSizeClass4());
      case kBranchNode: {
        for (int i = 0; i < leaf->size; ++i) {
          if (find_recurse(n2, HighsHashHelpers::hash(leaf->entries[i].key()),
                           hashPos, leaf->entries[i].key()))
            return &leaf->entries[i];
        }
      }
    }

    return nullptr;
  }

  static NodePtr removeChildFromBranchNode(BranchNode* branch, int location,
                                           uint64_t hash, int hashPos) {
    NodePtr newNode;
    int newNumChild = branch->occupation.num_set();

    // first check if we might be able to merge all children into one new leaf
    // based on the node numbers and assuming all of them might be in the
    // smallest size class
    if (newNumChild * InnerLeaf<1>::capacity() <= kLeafBurstThreshold) {
      // since we have a good chance of merging we now check the actual size
      // classes to see if that yields a number of entries at most the burst
      // threshold
      int childEntries = 0;
      for (int i = 0; i <= newNumChild; ++i) {
        childEntries += branch->child[i].numEntriesEstimate();
        if (childEntries > kLeafBurstThreshold) break;
      }

      if (childEntries < kLeafBurstThreshold) {
        // create a new merged inner leaf node containing all entries of
        // children first recompute the number of entries, but this time access
        // each child to get the actual number of entries needed and determine
        // this nodes size class since before we estimated the number of child
        // entries from the capacities of our child leaf node types which are
        // stored in the branch nodes pointers directly and avoid unnecessary
        // accesses of nodes that are not in cache.
        childEntries = 0;
        for (int i = 0; i <= newNumChild; ++i)
          childEntries += branch->child[i].numEntries();

        // check again if we exceed due to the extremely unlikely case
        // of having less than 5 list nodes with together more than 30 entries
        // as list nodes are only created in the last depth level
        if (childEntries < kLeafBurstThreshold) {
          switch ((childEntries + 1) / 8) {
            case 0: {
              InnerLeaf<1>* newLeafSize1 = new InnerLeaf<1>;
              newNode = newLeafSize1;
              for (int i = 0; i <= newNumChild; ++i)
                mergeIntoLeaf(newLeafSize1, hashPos, branch->child[i]);
              break;
            }
            case 1: {
              InnerLeaf<2>* newLeafSize2 = new InnerLeaf<2>;
              newNode = newLeafSize2;
              for (int i = 0; i <= newNumChild; ++i)
                mergeIntoLeaf(newLeafSize2, hashPos, branch->child[i]);
              break;
            }
            case 2: {
              InnerLeaf<3>* newLeafSize3 = new InnerLeaf<3>;
              newNode = newLeafSize3;
              for (int i = 0; i <= newNumChild; ++i)
                mergeIntoLeaf(newLeafSize3, hashPos, branch->child[i]);
              break;
            }
            case 3: {
              InnerLeaf<4>* newLeafSize4 = new InnerLeaf<4>;
              newNode = newLeafSize4;
              for (int i = 0; i <= newNumChild; ++i)
                mergeIntoLeaf(newLeafSize4, hashPos, branch->child[i]);
            }
          }

          destroyBranchingNode(branch);
          return newNode;
        }
      }
    }

    size_t newSize = getBranchNodeSize(newNumChild);
    size_t rightSize = (newNumChild - location) * sizeof(NodePtr);
    if (newSize == getBranchNodeSize(newNumChild + 1)) {
      // allocated size class is the same, so we do not allocate a new node
      memmove(&branch->child[location], &branch->child[location + 1],
              rightSize);
      newNode = branch;
    } else {
      // allocated size class changed, so we allocate a smaller branch node
      BranchNode* compressedBranch = (BranchNode*)::operator new(newSize);
      newNode = compressedBranch;

      size_t leftSize =
          offsetof(BranchNode, child) + location * sizeof(NodePtr);
      memcpy(compressedBranch, branch, leftSize);
      memcpy(&compressedBranch->child[location], &branch->child[location + 1],
             rightSize);

      destroyBranchingNode(branch);
    }

    return newNode;
  }

  NodePtr root;

  template <int SizeClass>
  bool insert_into_leaf(NodePtr* insertNode, InnerLeaf<SizeClass>* leaf,
                        uint64_t hash, int hashPos,
                        HighsHashTableEntry<K, V>& entry) {
    if (leaf->size == InnerLeaf<SizeClass>::capacity()) {
      if (leaf->find_entry(hash, hashPos, entry.key())) return false;

      InnerLeaf<SizeClass + 1>* newLeaf = new InnerLeaf<SizeClass + 1>;
      mergeIntoLeaf(newLeaf, leaf, hashPos);
      *insertNode = newLeaf;
      delete leaf;
      return newLeaf->insert_entry(hash, hashPos, entry);
    }

    return leaf->insert_entry(hash, hashPos, entry);
  }

  bool insert_recurse(NodePtr* insertNode, uint64_t hash, int hashPos,
                      HighsHashTableEntry<K, V>& entry) {
    switch (insertNode->getType()) {
      case kEmpty: {
        if (hashPos == kMaxDepth) {
          *insertNode = new ListLeaf(std::move(entry));
        } else {
          InnerLeaf<1>* leaf = new InnerLeaf<1>;
          *insertNode = leaf;
          leaf->insert_entry(hash, hashPos, entry);
        }
        return true;
      }
      case kListLeaf: {
        ListLeaf* leaf = insertNode->getListLeaf();
        ListNode* iter = &leaf->first;
        while (true) {
          // check for existing key
          if (iter->entry.key() == entry.key()) return false;

          if (iter->next == nullptr) {
            // reached the end of the list and key is not duplicate, so insert
            iter->next = new ListNode(std::move(entry));
            ++leaf->count;
            return true;
          }
          iter = iter->next;
        }

        break;
      }
      case kInnerLeafSizeClass1:
        return insert_into_leaf(insertNode,
                                insertNode->getInnerLeafSizeClass1(), hash,
                                hashPos, entry);
        break;
      case kInnerLeafSizeClass2:
        return insert_into_leaf(insertNode,
                                insertNode->getInnerLeafSizeClass2(), hash,
                                hashPos, entry);
        break;
      case kInnerLeafSizeClass3:
        return insert_into_leaf(insertNode,
                                insertNode->getInnerLeafSizeClass3(), hash,
                                hashPos, entry);
        break;
      case kInnerLeafSizeClass4: {
        InnerLeaf<4>* leaf = insertNode->getInnerLeafSizeClass4();
        if (leaf->size < InnerLeaf<4>::capacity())
          return leaf->insert_entry(hash, hashPos, entry);

        if (leaf->find_entry(hash, hashPos, entry.key())) return false;
        Occupation occupation = leaf->occupation;

        uint8_t hashChunk = get_hash_chunk(hash, hashPos);
        occupation.set(hashChunk);

        int branchSize = occupation.num_set();

        BranchNode* branch = createBranchingNode(branchSize);
        *insertNode = branch;
        branch->occupation = occupation;

        if (hashPos + 1 == kMaxDepth) {
          for (int i = 0; i < branchSize; ++i) branch->child[i] = nullptr;

          int pos = occupation.num_set_until(get_hash_chunk(hash, hashPos)) - 1;
          branch->child[pos] = new ListLeaf(std::move(entry));

          for (int i = 0; i < leaf->size; ++i) {
            if (branch->child[pos].getType() == kEmpty)
              branch->child[pos] = new ListLeaf(std::move(leaf->entries[i]));
            else {
              ListLeaf* listLeaf = branch->child[pos].getListLeaf();
              ListNode* newNode = new ListNode(std::move(listLeaf->first));
              listLeaf->first.next = newNode;
              listLeaf->first.entry = std::move(std::move(leaf->entries[i]));
              ++listLeaf->count;
            }
          }

          delete leaf;

          return true;
        }

        if (branchSize > 1) {
          uint64_t leafHashes[InnerLeaf<4>::capacity()];
          for (int i = 0; i < leaf->size; ++i)
            leafHashes[i] = HighsHashHelpers::hash(leaf->entries[i].key());

          // maxsize in one bucket = number of items - (num buckets-1)
          // since each bucket has at least 1 item the largest one can only
          // have all remaining ones After adding the item: If it does not
          // collid
          int maxEntriesPerLeaf = 2 + leaf->size - branchSize;

          if (maxEntriesPerLeaf <= InnerLeaf<1>::capacity()) {
            // all items can go into the smalles leaf size
            for (int i = 0; i < branchSize; ++i)
              branch->child[i] = new InnerLeaf<1>;

            int pos =
                occupation.num_set_until(get_hash_chunk(hash, hashPos)) - 1;
            branch->child[pos].getInnerLeafSizeClass1()->insert_entry(
                hash, hashPos + 1, entry);
            for (int i = 0; i < leaf->size; ++i) {
              pos = occupation.num_set_until(
                        get_hash_chunk(leafHashes[i], hashPos)) -
                    1;
              branch->child[pos].getInnerLeafSizeClass1()->insert_entry(
                  leafHashes[i], hashPos + 1, leaf->entries[i]);
            }

            delete leaf;
            return true;
          } else {
            // there are many collisions, determine the exact sizes first
            int8_t sizes[InnerLeaf<4>::capacity() + 1];
            memset(sizes, 0, branchSize);
            sizes[occupation.num_set_until(hashChunk) - 1] += 1;
            for (int i = 0; i < leaf->size; ++i) {
              int pos = occupation.num_set_until(
                            get_hash_chunk(leafHashes[i], hashPos)) -
                        1;
              sizes[pos] += 1;
            }

            for (int i = 0; i < branchSize; ++i) {
              switch ((sizes[i] + 1) / 8) {
                case 0:
                  branch->child[i] = new InnerLeaf<1>;
                  break;
                case 1:
                  branch->child[i] = new InnerLeaf<2>;
                  break;
                case 2:
                  branch->child[i] = new InnerLeaf<3>;
                  break;
                case 3:
                  branch->child[i] = new InnerLeaf<4>;
                  break;
              }
            }

            for (int i = 0; i < leaf->size; ++i) {
              int pos = occupation.num_set_until(
                            get_hash_chunk(leafHashes[i], hashPos)) -
                        1;

              switch (branch->child[pos].getType()) {
                case kInnerLeafSizeClass1:
                  branch->child[pos].getInnerLeafSizeClass1()->insert_entry(
                      leafHashes[i], hashPos + 1, leaf->entries[i]);
                  break;
                case kInnerLeafSizeClass2:
                  branch->child[pos].getInnerLeafSizeClass2()->insert_entry(
                      leafHashes[i], hashPos + 1, leaf->entries[i]);
                  break;
                case kInnerLeafSizeClass3:
                  branch->child[pos].getInnerLeafSizeClass3()->insert_entry(
                      leafHashes[i], hashPos + 1, leaf->entries[i]);
                  break;
                case kInnerLeafSizeClass4:
                  branch->child[pos].getInnerLeafSizeClass4()->insert_entry(
                      leafHashes[i], hashPos + 1, leaf->entries[i]);
              }
            }
          }

          delete leaf;

          int pos = occupation.num_set_until(hashChunk) - 1;
          insertNode = &branch->child[pos];
          ++hashPos;
        } else {
          // extremely unlikely that the new branch node only gets one
          // child in that case create it and defer the insertion into
          // another step
          branch->child[0] = leaf;
          insertNode = &branch->child[0];
          ++hashPos;
        }

        break;
      }
      case kBranchNode: {
        BranchNode* branch = insertNode->getBranchNode();

        int location =
            branch->occupation.num_set_until(get_hash_chunk(hash, hashPos));

        if (branch->occupation.test(get_hash_chunk(hash, hashPos))) {
          --location;
        } else {
          branch = addChildToBranchNode(branch, get_hash_chunk(hash, hashPos),
                                        location);

          branch->child[location] = nullptr;
          branch->occupation.set(get_hash_chunk(hash, hashPos));
        }

        *insertNode = branch;
        insertNode = &branch->child[location];
        ++hashPos;
      }
    }

    return insert_recurse(insertNode, hash, hashPos, entry);
  }

  void erase_recurse(NodePtr* erase_node, uint64_t hash, int hashPos,
                     const K& key) {
    switch (erase_node->getType()) {
      case kEmpty: {
        return;
      }
      case kListLeaf: {
        ListLeaf* leaf = erase_node->getListLeaf();

        // check for existing key
        ListNode* iter = &leaf->first;

        do {
          ListNode* next = iter->next;
          if (iter->entry.key() == key) {
            // key found, decrease count
            --leaf->count;
            if (next != nullptr) {
              // if we have a next node after replace the entry in iter by
              // moving that node into it
              *iter = std::move(*next);
              // delete memory of that node
              delete next;
            }

            break;
          }

          iter = next;
        } while (iter != nullptr);

        if (leaf->count == 0) {
          delete leaf;
          *erase_node = nullptr;
        }

        return;
      }
      case kInnerLeafSizeClass1: {
        InnerLeaf<1>* leaf = erase_node->getInnerLeafSizeClass1();
        if (leaf->erase_entry(hash, hashPos, key)) {
          if (leaf->size == 0) {
            delete leaf;
            *erase_node = nullptr;
          }
        }

        return;
      }
      case kInnerLeafSizeClass2: {
        InnerLeaf<2>* leaf = erase_node->getInnerLeafSizeClass2();

        if (leaf->erase_entry(hash, hashPos, key)) {
          if (leaf->size == InnerLeaf<1>::capacity()) {
            InnerLeaf<1>* newLeaf = new InnerLeaf<1>;
            *erase_node = newLeaf;
            mergeIntoLeaf(newLeaf, leaf, hashPos);
            delete leaf;
          }
        }

        return;
      }
      case kInnerLeafSizeClass3: {
        InnerLeaf<3>* leaf = erase_node->getInnerLeafSizeClass3();

        if (leaf->erase_entry(hash, hashPos, key)) {
          if (leaf->size == InnerLeaf<2>::capacity()) {
            InnerLeaf<2>* newLeaf = new InnerLeaf<2>;
            *erase_node = newLeaf;
            mergeIntoLeaf(newLeaf, leaf, hashPos);
            delete leaf;
          }
        }

        return;
      }
      case kInnerLeafSizeClass4: {
        InnerLeaf<4>* leaf = erase_node->getInnerLeafSizeClass4();

        if (leaf->erase_entry(hash, hashPos, key)) {
          if (leaf->size == InnerLeaf<3>::capacity()) {
            InnerLeaf<3>* newLeaf = new InnerLeaf<3>;
            *erase_node = newLeaf;
            mergeIntoLeaf(newLeaf, leaf, hashPos);
            delete leaf;
          }
        }

        return;
      }
      case kBranchNode: {
        BranchNode* branch = erase_node->getBranchNode();

        if (!branch->occupation.test(get_hash_chunk(hash, hashPos))) return;

        int location =
            branch->occupation.num_set_until(get_hash_chunk(hash, hashPos)) - 1;
        erase_recurse(&branch->child[location], hash, hashPos + 1, key);

        if (branch->child[location].getType() != kEmpty) return;

        branch->occupation.flip(get_hash_chunk(hash, hashPos));

        *erase_node =
            removeChildFromBranchNode(branch, location, hash, hashPos);
        break;
      }
    }
  }

  static const ValueType* find_recurse(NodePtr node, uint64_t hash, int hashPos,
                                       const K& key) {
    int startPos = hashPos;
    switch (node.getType()) {
      case kEmpty:
        return nullptr;
      case kListLeaf: {
        ListLeaf* leaf = node.getListLeaf();
        ListNode* iter = &leaf->first;
        do {
          if (iter->entry.key() == key) return &iter->entry.value();
          iter = iter->next;
        } while (iter != nullptr);
        return nullptr;
      }
      case kInnerLeafSizeClass1: {
        InnerLeaf<1>* leaf = node.getInnerLeafSizeClass1();
        return leaf->find_entry(hash, hashPos, key);
      }
      case kInnerLeafSizeClass2: {
        InnerLeaf<2>* leaf = node.getInnerLeafSizeClass2();
        return leaf->find_entry(hash, hashPos, key);
      }
      case kInnerLeafSizeClass3: {
        InnerLeaf<3>* leaf = node.getInnerLeafSizeClass3();
        return leaf->find_entry(hash, hashPos, key);
      }
      case kInnerLeafSizeClass4: {
        InnerLeaf<4>* leaf = node.getInnerLeafSizeClass4();
        return leaf->find_entry(hash, hashPos, key);
      }
      case kBranchNode: {
        BranchNode* branch = node.getBranchNode();
        if (!branch->occupation.test(get_hash_chunk(hash, hashPos)))
          return nullptr;
        int location =
            branch->occupation.num_set_until(get_hash_chunk(hash, hashPos)) - 1;
        node = branch->child[location];
        ++hashPos;
      }
    }

    assert(hashPos > startPos);

    return find_recurse(node, hash, hashPos, key);
  }

  static const HighsHashTableEntry<K, V>* find_common_recurse(NodePtr n1,
                                                              NodePtr n2,
                                                              int hashPos) {
    if (n1.getType() > n2.getType()) std::swap(n1, n2);

    switch (n1.getType()) {
      case kEmpty:
        return nullptr;
      case kListLeaf: {
        ListLeaf* leaf = n1.getListLeaf();
        ListNode* iter = &leaf->first;
        do {
          if (find_recurse(n2, HighsHashHelpers::hash(iter->entry.key()),
                           hashPos, iter->entry.key()))
            return &iter->entry;
          iter = iter->next;
        } while (iter != nullptr);
        return nullptr;
      }
      case kInnerLeafSizeClass1:
        return findCommonInLeaf(n1.getInnerLeafSizeClass1(), n2, hashPos);
      case kInnerLeafSizeClass2:
        return findCommonInLeaf(n1.getInnerLeafSizeClass2(), n2, hashPos);
      case kInnerLeafSizeClass3:
        return findCommonInLeaf(n1.getInnerLeafSizeClass3(), n2, hashPos);
      case kInnerLeafSizeClass4:
        return findCommonInLeaf(n1.getInnerLeafSizeClass4(), n2, hashPos);
      case kBranchNode: {
        BranchNode* branch1 = n1.getBranchNode();
        BranchNode* branch2 = n2.getBranchNode();

        uint64_t matchMask = branch1->occupation & branch2->occupation;

        while (matchMask) {
          int pos = HighsHashHelpers::log2i(matchMask);
          assert((branch1->occupation >> pos) & 1);
          assert((branch2->occupation >> pos) & 1);
          assert((matchMask >> pos) & 1);

          matchMask ^= (uint64_t{1} << pos);

          assert(((matchMask >> pos) & 1) == 0);

          int location1 = branch1->occupation.num_set_until(pos) - 1;
          int location2 = branch2->occupation.num_set_until(pos) - 1;

          const HighsHashTableEntry<K, V>* match =
              find_common_recurse(branch1->child[location1],
                                  branch2->child[location2], hashPos + 1);
          if (match != nullptr) return match;
        }

        return nullptr;
      }
    }
  }

  static void destroy_recurse(NodePtr node) {
    switch (node.getType()) {
      case kListLeaf: {
        ListLeaf* leaf = node.getListLeaf();
        ListNode* iter = leaf->first.next;
        delete leaf;
        while (iter != nullptr) {
          ListNode* next = iter->next;
          delete iter;
          iter = iter->next;
        }

        break;
      }
      case kInnerLeafSizeClass1:
        delete node.getInnerLeafSizeClass1();
        break;
      case kInnerLeafSizeClass2:
        delete node.getInnerLeafSizeClass2();
        break;
      case kInnerLeafSizeClass3:
        delete node.getInnerLeafSizeClass3();
        break;
      case kInnerLeafSizeClass4:
        delete node.getInnerLeafSizeClass4();
        break;
      case kBranchNode: {
        BranchNode* branch = node.getBranchNode();
        int size = branch->occupation.num_set();

        for (int i = 0; i < size; ++i) destroy_recurse(branch->child[i]);

        destroyBranchingNode(branch);
      }
    }
  }

  static NodePtr copy_recurse(NodePtr node) {
    switch (node.getType()) {
      case kEmpty:
        assert(false);
        break;
      case kListLeaf: {
        ListLeaf* leaf = node.getListLeaf();

        ListLeaf* copyLeaf = new ListLeaf(*leaf);

        ListNode* iter = &leaf->first;
        ListNode* copyIter = &copyLeaf->first;
        do {
          copyIter->next = new ListNode(*iter->next);
          iter = iter->next;
          copyIter = copyIter->next;
        } while (iter->next != nullptr);

        return copyLeaf;
      }
      case kInnerLeafSizeClass1: {
        InnerLeaf<1>* leaf = node.getInnerLeafSizeClass1();
        return new InnerLeaf<1>(*leaf);
      }
      case kInnerLeafSizeClass2: {
        InnerLeaf<2>* leaf = node.getInnerLeafSizeClass2();
        return new InnerLeaf<2>(*leaf);
      }
      case kInnerLeafSizeClass3: {
        InnerLeaf<3>* leaf = node.getInnerLeafSizeClass3();
        return new InnerLeaf<3>(*leaf);
      }
      case kInnerLeafSizeClass4: {
        InnerLeaf<4>* leaf = node.getInnerLeafSizeClass4();
        return new InnerLeaf<4>(*leaf);
      }
      case kBranchNode: {
        BranchNode* branch = node.getBranchNode();
        int size = branch->occupation.num_set();
        BranchNode* newBranch =
            (BranchNode*)::operator new(getBranchNodeSize(size));
        newBranch->occupation = branch->occupation;
        for (int i = 0; i < size; ++i)
          newBranch->child[i] = copy_recurse(branch->child[i]);

        return newBranch;
      }
    }
  }

  template <typename F>
  static bool for_each_recurse(NodePtr node, F&& f) {
    switch (node.getType()) {
      case kEmpty:
        break;
      case kListLeaf: {
        ListLeaf* leaf = node.getListLeaf();
        ListNode* iter = &leaf->first;
        do {
          if (f(iter->entry)) return true;
          iter = iter->next;
        } while (iter != nullptr);
        break;
      }
      case kInnerLeafSizeClass1: {
        InnerLeaf<1>* leaf = node.getInnerLeafSizeClass1();
        for (int i = 0; i < leaf->size; ++i)
          if (f(leaf->entries[i])) return true;

        break;
      }
      case kInnerLeafSizeClass2: {
        InnerLeaf<2>* leaf = node.getInnerLeafSizeClass2();
        for (int i = 0; i < leaf->size; ++i)
          if (f(leaf->entries[i])) return true;

        break;
      }
      case kInnerLeafSizeClass3: {
        InnerLeaf<3>* leaf = node.getInnerLeafSizeClass3();
        for (int i = 0; i < leaf->size; ++i)
          if (f(leaf->entries[i])) return true;

        break;
      }
      case kInnerLeafSizeClass4: {
        InnerLeaf<4>* leaf = node.getInnerLeafSizeClass4();
        for (int i = 0; i < leaf->size; ++i)
          if (f(leaf->entries[i])) return true;

        break;
      }
      case kBranchNode: {
        BranchNode* branch = node.getBranchNode();
        int size = branch->occupation.num_set();

        for (int i = 0; i < size; ++i)
          if (for_each_recurse(branch->child[i], f)) return true;
      }
    }

    return false;
  }

 public:
  template <typename... Args>
  bool insert(Args&&... args) {
    HighsHashTableEntry<K, V> entry(std::forward<Args>(args)...);
    uint64_t hash = HighsHashHelpers::hash(entry.key());
    return insert_recurse(&root, hash, 0, entry);
  }

  void erase(const K& key) {
    uint64_t hash = HighsHashHelpers::hash(key);

    erase_recurse(&root, hash, 0, key);
  }

  bool contains(const K& key) const {
    uint64_t hash = HighsHashHelpers::hash(key);
    return find_recurse(root, hash, 0, key) != nullptr;
  }

  const ValueType* find(const K& key) const {
    uint64_t hash = HighsHashHelpers::hash(key);

    return find_recurse(root, hash, 0, key);
  }

  const HighsHashTableEntry<K, V>* find_common(
      const HighsHashTree<K, V>& other) const {
    return find_common_recurse(root, other.root, 0);
  }

  bool empty() const { return root.getType() == kEmpty; }

  void clear() {
    destroy_recurse(root);
    root = nullptr;
  }

  template <typename F>
  bool for_each(F&& f) const {
    return for_each_recurse(root, f);
  }

  HighsHashTree() = default;

  HighsHashTree(HighsHashTree&& other) : root(other.root) {
    other.root = nullptr;
  }

  HighsHashTree(const HighsHashTree& other) : root(copy_recurse(other.root)) {}

  HighsHashTree& operator=(HighsHashTree&& other) {
    destroy_recurse(root);
    root = other.root;
    other.root = nullptr;
    return *this;
  }

  HighsHashTree& operator=(const HighsHashTree& other) {
    destroy_recurse(root);
    root = copy_recurse(other.root);
    return *this;
  }

  ~HighsHashTree() { destroy_recurse(root); }
};

#endif