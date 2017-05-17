#include <mutex>
#include <atomic>
#include <vector>


template <size_t SBSize, uint8_t FullnesKinds>
class MtAllocator {
public:

private:
    class Heap;

    class SuperBlock {
    public:
        SuperBlock(const size_t block_size_log2_)
                : block_size_log2_(block_size_log2_)
                , block_size_(1 << block_size_log2_)
        {
            unsigned short blocks_number = SBSize / block_size_;

            data_ = malloc(sizeof(unsigned short) * blocks_number +
                           sizeof(SuperBlock*) * blocks_number + SBSize);
            next_block_ = (unsigned short*) data_;
            blocks_ = (uint8_t*)next_block_+ blocks_number;

            for(unsigned short i = 0; i < blocks_number; ++i){
                next_block_[i] = i + 1;
                ((SuperBlock**)(blocks_ + i * (block_size_ + sizeof(SuperBlock*))))[0] = this;
            }


        }

        virtual ~SuperBlock() {
            free(data_);
        }

        const size_t GetBlock_size() const {
            return block_size_;
        }

        const size_t GetBlock_size_log2() const {
            return block_size_log2_;
        }

        size_t GetUsed() const {
            return used_;
        }

        const std::atomic<Heap *> &GetOwner() const {
            return owner_;
        }

        SuperBlock *GetNext() const {
            return next_;
        }

        SuperBlock *GetPrev() const {
            return prev_;
        }

        void SetOwner(const std::atomic<Heap *> &owner_) {
            SuperBlock::owner_ = owner_;
        }

        void SetNext(SuperBlock *next_) {
            SuperBlock::next_ = next_;
        }

        void SetPrev(SuperBlock *prev_) {
            SuperBlock::prev_ = prev_;
        }

        void lock(){
            lock_.lock();
        }

        void unlock(){
            lock_.unlock();
        }

        void* GetBlock() {
            void* block = GetBlockAddress(free_block_);

            free_block_ = next_block_[free_block_];
            used_ += block_size_;

            return block;
        }

        void FreeBlock(void* address) {
            unsigned short block = ((uint8_t *)address - blocks_ - sizeof(SuperBlock*)) /
                    (block_size_ + sizeof(SuperBlock*));

            next_block_[block] = free_block_;
            free_block_ = block;
            used_ -= block_size_;
        }


    private:
        std::mutex lock_;

        const size_t block_size_;
        const size_t block_size_log2_;

        size_t used_ = 0;

        std::atomic<Heap*> owner_ = nullptr;

        SuperBlock* next_ = nullptr;
        SuperBlock* prev_ = nullptr;

        void* data_;

        uint8_t* blocks_;
        unsigned short* next_block_;
        unsigned short free_block_ = 0;

        void* GetBlockAddress(size_t id) {
            return blocks_ + id * block_size_ + (id + 1) * sizeof(SuperBlock*);
        }
    };

    class Heap{
    public:
        Heap()
            : superblocks_(Log2(SBSize), std::vector<SuperBlock*>(FullnesKinds + 2, nullptr))
        {}

        virtual ~Heap() {
            for(auto& f_suberblocks : superblocks_)
                for(auto& superblock : f_suberblocks)
                    while(superblock != nullptr) {
                        auto next = superblock->getNext();
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

    private:
        std::mutex lock_;

        std::vector<std::vector<SuperBlock*>> superblocks_;

        size_t allocated_ = 0;
        size_t used_ = 0;
    };

    static size_t Log2(size_t n) {
        size_t log2 = 0;

        for(size_t m = 1; m < n; m <<= 1)
            ++log2;

        return log2;
    }
};

void* mtalloc (size_t bytes){

}

void mtfree (void* ptr){

}

int main () {
    return 0;
}