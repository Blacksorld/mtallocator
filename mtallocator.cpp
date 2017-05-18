#include <mutex>
#include <atomic>
#include <vector>
#include <thread>


template<size_t SBSize>
class MtAllocator {
public:
    MtAllocator(const size_t heaps_number)
            : heaps_(1 + heaps_number) {}

    void* Allocate(const size_t size) {
        if (2 * size > SBSize)
            return AllocateLarge(size);

        const size_t size_log2 = Log2(size);
        Heap& heap = GetHeap();
        std::lock_guard<Heap> thread_heap_lock(heap);

        void* block = heap.Allocate(size_log2);
        if (block != nullptr)
            return block;

        std::unique_lock<Heap> global_heap_lock(heaps_[0]);
        SuperBlock* superblock = heaps_[0].ReleaseSuperblock(size_log2);
        global_heap_lock.unlock();

        if (superblock == nullptr)
            superblock = new SuperBlock(size_log2);

        block = superblock->GetBlock();
        heap.AcquireSuperblock(superblock);

        return block;
    }

    void Free(void* ptr) {
        if (ptr == nullptr)
            return;

        SuperBlock* superblock = GetBlockOwner(ptr);
        if (superblock == nullptr) {
            FreeLarge(ptr);
            return;
        }

        Heap* heap = superblock->GetOwner();
        std::unique_lock<Heap> heap_lock(*heap);
        while(heap != superblock->GetOwner()){
            heap_lock.unlock();
            heap = superblock->GetOwner();
            heap_lock = std::unique_lock<Heap>(*heap);
        }

        heap->Free(ptr, superblock);

        if(heap == &heaps_[0])
            return;

        auto allocated = heap->GetAllocated();
        auto used = heap->GetUsed();

        if(used < allocated - SBSize && 4 * used < 3 * allocated){
            std::lock_guard<Heap> global_heap_lock(heaps_[0]);
            SuperBlock* sb = heap->ReleaseSuperblock();
            heaps_[0].AcquireSuperblock(sb);
        }
    }

private:
    class Heap;

    class SuperBlock {
    public:
        SuperBlock(const size_t block_size_log2)
                : block_size_log2_(block_size_log2)
                , block_size_(1 << block_size_log2)
                , owner_(nullptr) {
            unsigned short blocks_number = SBSize / block_size_;

            data_ = malloc((sizeof(unsigned short) + sizeof(SuperBlock*)) * blocks_number
                           + SBSize);
            next_block_ = (unsigned short*) data_;
            blocks_ = (uint8_t*) &next_block_[blocks_number];

            for (unsigned short i = 0; i < blocks_number; ++i) {
                next_block_[i] = i + 1;
                ((SuperBlock**) (blocks_ + i * (block_size_ + sizeof(SuperBlock*))))[0] = this;
            }
            next_block_[blocks_number - 1] = -1;
        }

        virtual ~SuperBlock() {
            free(data_);
        }

        size_t GetBlockSize() const {
            return block_size_;
        }

        size_t GetBlockSizeLog2() const {
            return block_size_log2_;
        }

        size_t GetUsed() const {
            return used_;
        }

        Heap* GetOwner() const {
            return owner_;
        }

        SuperBlock* GetNext() const {
            return next_;
        }

        SuperBlock* GetPrev() const {
            return prev_;
        }

        void SetOwner(Heap* owner) {
            owner_ = owner;
        }

        void SetNext(SuperBlock* next_) {
            SuperBlock::next_ = next_;
        }

        void SetPrev(SuperBlock* prev_) {
            SuperBlock::prev_ = prev_;
        }

        void* GetBlock() {
            void* block = GetBlockAddress(free_block_);

            free_block_ = next_block_[free_block_];
            used_ += block_size_;

            return block;
        }

        void FreeBlock(void* address) {
            unsigned short block = ((uint8_t*) address - blocks_ - sizeof(SuperBlock*)) /
                                   (block_size_ + sizeof(SuperBlock*));

            next_block_[block] = free_block_;
            free_block_ = block;
            used_ -= block_size_;
        }

        void Pop() {
            if (prev_ != nullptr)
                prev_->next_ = next_;
            if (next_ != nullptr)
                next_->prev_ = prev_;

            prev_ = next_ = nullptr;
        }

        void Push(SuperBlock* superblock) {
            superblock->next_ = this;
            prev_ = superblock;
        }


    private:
        const size_t block_size_log2_;
        const size_t block_size_;


        size_t used_ = 0;

        std::atomic<Heap*> owner_;

        SuperBlock* next_ = nullptr;
        SuperBlock* prev_ = nullptr;

        void* data_;

        uint8_t* blocks_;
        unsigned short* next_block_;
        unsigned short free_block_ = 0;

        void* GetBlockAddress(size_t id) const {
            return blocks_ + id * block_size_ + (id + 1) * sizeof(SuperBlock*);
        }
    };

    class Heap {
    public:
        Heap()
                : superblocks_(Log2(SBSize), nullptr) {}

        virtual ~Heap() {
            for (auto& superblock : superblocks_)
                while (superblock != nullptr) {
                    auto next = superblock->GetNext();
                    delete superblock;
                    superblock = next;
                }
        }

        size_t GetAllocated() const {
            return allocated_;
        }

        size_t GetUsed() const {
            return used_;
        }

        void lock() {
            lock_.lock();
        }

        void unlock() {
            lock_.unlock();
        }

        void* Allocate(const size_t size_log2) {
            SuperBlock* superblock = nullptr;

            for (SuperBlock* superblocks = superblocks_[size_log2]; superblocks != nullptr;
                 superblocks = superblocks->GetNext())
                if (superblocks->GetUsed()< SBSize) {
                    superblock = superblocks;
                    break;
                }

            if (superblock == nullptr)
                return nullptr;

            void* block = superblock->GetBlock();
            used_ += superblock->GetBlockSize();
            return block;
        }

        void Free(void* block, SuperBlock* superblock) {
            superblock->FreeBlock(block);
            used_ -= superblock->GetBlockSize();
        }

        SuperBlock* ReleaseSuperblock(const size_t size_log2) {
            SuperBlock* superblock = nullptr;

            for (SuperBlock* superblocks = superblocks_[size_log2]; superblocks != nullptr;
                 superblocks = superblocks->GetNext()) {
                size_t used = superblocks->GetUsed();
                if (used < SBSize) {
                    superblock = superblocks;
                    break;
                }
            }

            if (superblock == nullptr)
                return nullptr;

            PopSuperblock(superblock);
            allocated_ -= SBSize;
            used_ -= superblock->GetUsed();

            return superblock;
        }

        SuperBlock* ReleaseSuperblock() {
            SuperBlock* superblock = nullptr;

            for(auto superblocks : superblocks_)
                for (; superblocks != nullptr; superblocks = superblocks->GetNext()) {
                    size_t used = superblocks->GetUsed();
                    if (used < SBSize) {
                        superblock = superblocks;
                        break;
                    }
                }

            if (superblock == nullptr)
                return nullptr;

            PopSuperblock(superblock);
            allocated_ -= SBSize;
            used_ -= superblock->GetUsed();

            return superblock;
        }

        void AcquireSuperblock(SuperBlock* superblock) {
            superblock->SetOwner(this);
            PushSuperblock(superblock);

            allocated_ += SBSize;
            used_ += superblock->GetUsed();
        }

    private:
        void PopSuperblock(SuperBlock* superblock) {
            if(superblock->GetPrev() == nullptr)
                superblocks_[superblock->GetBlockSizeLog2()] = superblock->GetNext();
            superblock->Pop();
        }

        void PushSuperblock(SuperBlock* superblock) {
            auto& superblocks = superblocks_[superblock->GetBlockSizeLog2()];

            if (superblocks == nullptr) {
                superblocks = superblock;
                return;
            }

            superblocks->Push(superblock);
            superblocks = superblock;
        }

        std::mutex lock_;

        std::vector<SuperBlock*> superblocks_;

        size_t allocated_ = 0;
        size_t used_ = 0;
    };

    SuperBlock* GetBlockOwner(void* block) const {
        return ((SuperBlock**) block)[-1];
    }

    static size_t Log2(size_t n) {
        size_t log2 = 0;

        for (size_t m = 1; m < n; m <<= 1)
            ++log2;

        return log2;
    }

    void* AllocateLarge(const size_t size) {
        void* data = malloc(size + sizeof(SuperBlock*));
        ((SuperBlock**) data)[0] = nullptr;
        return (uint8_t*) data + sizeof(SuperBlock*);
    }

    void FreeLarge(void* ptr) {
        free((uint8_t*) ptr - sizeof(SuperBlock*));
    }

    Heap& GetHeap() {
        return heaps_[hasher_(std::this_thread::get_id()) % (heaps_.size() - 1) + 1];
    }

    std::vector<Heap> heaps_;
    std::hash<std::thread::id> hasher_;
};

static MtAllocator<1 << 15>& GetAllocator() {
    static size_t heaps_number = std::max(std::thread::hardware_concurrency() * 2, 1U);
    static MtAllocator<1 << 15> allocator(heaps_number);

    return allocator;
}

void* mtalloc(size_t bytes) {
    return GetAllocator().Allocate(bytes);
}

void mtfree(void* ptr) {
    GetAllocator().Free(ptr);
}