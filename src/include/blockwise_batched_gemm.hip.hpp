#pragma once
#include "threadwise_gemm.hip.hpp"

template <index_t BlockSize,
          class BlockMatrixA,
          class BlockMatrixB,
          class ThreadMatrixC,
          bool TransA,
          bool TransB,
          bool TransC,
          index_t BlockMatrixStrideA,
          index_t BlockMatrixStrideB,
          index_t ThreadMatrixStrideC,
          index_t BatchSize,
          index_t BatchPerThread,
          index_t KPerThreadLoop,
          bool DistributeThreadAlongColumnFirst>
struct Blockwise1dStridedBatchedGemmBlockABlockBThreadC
{
    index_t mMyThreadOffsetA = 0;
    index_t mMyThreadOffsetB = 0;

    struct MatrixIndex
    {
        index_t batch;
        index_t row;
        index_t col;
    };

    __device__ Blockwise1dStridedBatchedGemmBlockABlockBThreadC()
    {
        constexpr auto a_block_mtx = BlockMatrixA{};
        constexpr auto b_block_mtx = BlockMatrixB{};

        const auto c_thread_mtx_index = GetBeginOfThreadMatrixC(get_thread_local_1d_id());

        mMyThreadOffsetA = c_thread_mtx_index.batch * BlockMatrixStrideA +
                           ((!TransA) ? a_block_mtx.Get1dIndex(c_thread_mtx_index.row, 0)
                                      : a_block_mtx.Get1dIndex(0, c_thread_mtx_index.row));

        mMyThreadOffsetB = c_thread_mtx_index.batch * BlockMatrixStrideB +
                           ((!TransB) ? b_block_mtx.Get1dIndex(0, c_thread_mtx_index.col)
                                      : b_block_mtx.Get1dIndex(c_thread_mtx_index.col, 0));

#if 0
        if(get_thread_local_1d_id() == 0 && get_block_1d_id() == 0)
        {
            print_ConstantMatrixDescriptor(BlockMatrixA{}, "a_block_mtx: ");
            print_ConstantMatrixDescriptor(BlockMatrixB{}, "b_block_mtx: ");
            print_ConstantMatrixDescriptor(ThreadMatrixC{}, "c_thread_mtx: ");

            printf("%u %u, %u %u %u, %u %u\n",
                   get_block_1d_id(),
                   get_thread_local_1d_id(),
                   c_thread_mtx_index.batch,
                   c_thread_mtx_index.row,
                   c_thread_mtx_index.col,
                   mMyThreadOffsetA,
                   mMyThreadOffsetB);
        }
#endif
    }

    __device__ MatrixIndex GetBeginOfThreadMatrixC(index_t thread_id) const
    {

        if(TransA && (!TransB) && (!TransC))
        {
            constexpr auto a_block_mtx = BlockMatrixA{};
            constexpr auto b_block_mtx = BlockMatrixB{};

            static_assert(a_block_mtx.NRow() == b_block_mtx.NRow(),
                          "wrong! k dimension not consistent!");

            constexpr index_t MPerBlock = a_block_mtx.NCol();
            constexpr index_t NPerBlock = b_block_mtx.NCol();

            constexpr auto c_thread_mtx = ThreadMatrixC{};

            // divide thread work
            constexpr index_t MPerThread = c_thread_mtx.NRow();
            constexpr index_t NPerThread = c_thread_mtx.NCol();

            static_assert(BatchSize % BatchPerThread == 0, "BatchSize % BatchPerThread != 0");
            static_assert(MPerBlock % MPerThread == 0, "MPerBlock % MPerThread != 0");
            static_assert(NPerBlock % NPerThread == 0, "NPerBlock % NPerThread != 0");

            constexpr index_t BatchThreadWork = (BatchSize + BatchPerThread - 1) / BatchPerThread;
            constexpr index_t MThreadWork     = (MPerBlock + MPerThread - 1) / MPerThread;
            constexpr index_t NThreadWork     = (NPerBlock + NPerThread - 1) / NPerThread;

            static_assert(BlockSize == BatchThreadWork * MThreadWork * NThreadWork,
                          "wrong! wrong BlockSize");

            if(DistributeThreadAlongColumnFirst)
            {
                // num of operations can be reduced
                const index_t b_work_id = thread_id / (MThreadWork * NThreadWork);
                index_t itmp            = thread_id - b_work_id * (MThreadWork * NThreadWork);
                const index_t m_work_id = itmp / NThreadWork;
                const index_t n_work_id = itmp - m_work_id * NThreadWork;

                return MatrixIndex{
                    b_work_id * BatchPerThread, m_work_id * MPerThread, n_work_id * NPerThread};
            }
            else
            {
                // not implemented
                assert(false);
            }
        }
        else
        {
            // not implemented
            assert(false);
        }
    }

    // this should be optimized away if input is known
    __device__ static MatrixIndex
    GetDistanceFromBeginOfThreadMatrixC(index_t batch_in_c, index_t m_in_c, index_t n_in_c)
    {
        return MatrixIndex{batch_in_c, m_in_c, n_in_c};
    }

    template <class FloatA, class FloatB, class FloatC, class Accumulator>
    __device__ void Run(const FloatA* __restrict__ p_a_block,
                        const FloatB* __restrict__ p_b_block,
                        FloatC* __restrict__ p_c_thread,
                        Accumulator f_accum) const
    {
        if(TransA && (!TransB) && (!TransC))
        {
            constexpr auto True  = integral_constant<bool, true>{};
            constexpr auto False = integral_constant<bool, false>{};

            constexpr auto a_block_mtx  = BlockMatrixA{};
            constexpr auto b_block_mtx  = BlockMatrixB{};
            constexpr auto c_thread_mtx = ThreadMatrixC{};

            constexpr index_t KPerBlock = a_block_mtx.NRow(); // A is transposed

            constexpr index_t MPerThread = c_thread_mtx.NRow();
            constexpr index_t NPerThread = c_thread_mtx.NCol();

            // a is transposed, b is not
            constexpr auto a_thread_mtx =
                make_ConstantMatrixDescriptor(Number<KPerThreadLoop>{}, Number<MPerThread>{});

            constexpr auto b_thread_mtx =
                make_ConstantMatrixDescriptor(Number<KPerThreadLoop>{}, Number<NPerThread>{});

            FloatA p_a_thread[a_thread_mtx.GetElementSpace()];
            FloatB p_b_thread[b_thread_mtx.GetElementSpace()];

            // loop over k
            for(index_t k_begin = 0; k_begin < KPerBlock; k_begin += KPerThreadLoop)
            {
                // read first batch of a, b
                threadwise_matrix_copy(a_block_mtx,
                                       p_a_block + mMyThreadOffsetA +
                                           k_begin * a_block_mtx.RowStride(),
                                       a_thread_mtx,
                                       p_a_thread,
                                       a_thread_mtx.GetLengths());

                threadwise_matrix_copy(b_block_mtx,
                                       p_b_block + mMyThreadOffsetB +
                                           k_begin * b_block_mtx.RowStride(),
                                       b_thread_mtx,
                                       p_b_thread,
                                       b_thread_mtx.GetLengths());

                // loop over batch
                for(index_t ib = 0; ib + 1 < BatchPerThread; ++ib)
                {
                    // do current batch of gemm
                    threadwise_gemm(a_thread_mtx,
                                    True,
                                    p_a_thread,
                                    b_thread_mtx,
                                    False,
                                    p_b_thread,
                                    c_thread_mtx,
                                    False,
                                    p_c_thread + ib * ThreadMatrixStrideC,
                                    f_accum);

                    // read next batch of a, b
                    if(BlockMatrixStrideA != 0)
                    {
                        threadwise_matrix_copy(a_block_mtx,
                                               p_a_block + mMyThreadOffsetA +
                                                   (ib + 1) * BlockMatrixStrideA +
                                                   +k_begin * a_block_mtx.RowStride(),
                                               a_thread_mtx,
                                               p_a_thread,
                                               a_thread_mtx.GetLengths());
                    }

                    if(BlockMatrixStrideB != 0)
                    {
                        threadwise_matrix_copy(b_block_mtx,
                                               p_b_block + mMyThreadOffsetB +
                                                   (ib + 1) * BlockMatrixStrideB +
                                                   k_begin * b_block_mtx.RowStride(),
                                               b_thread_mtx,
                                               p_b_thread,
                                               b_thread_mtx.GetLengths());
                    }
                }

                // do last batch of gemm
                threadwise_gemm(a_thread_mtx,
                                True,
                                p_a_thread,
                                b_thread_mtx,
                                False,
                                p_b_thread,
                                c_thread_mtx,
                                False,
                                p_c_thread + (BatchPerThread - 1) * ThreadMatrixStrideC,
                                f_accum);
            }
        }
    }
};

template <index_t BlockSize,
          class BlockMatrixA,
          class BlockMatrixB,
          class ThreadMatrixC,
          index_t BlockMatrixStrideA,
          index_t BlockMatrixStrideB,
          index_t ThreadMatrixStrideC,
          index_t BatchSize,
          index_t MPerThreadSubC,
          index_t NPerThreadSubC,
          index_t MLevel0Cluster,
          index_t NLevel0Cluster,
          index_t MLevel1Cluster,
          index_t NLevel1Cluster,
          index_t KPerThreadLoop,
          index_t BatchPerThread>
struct BlockwiseBatchGemmBlockABlockBThreadCTransANormalBNormalC_V2
{
    index_t mMyThreadOffsetA = 0;
    index_t mMyThreadOffsetB = 0;

    struct MatrixIndex
    {
        index_t batch;
        index_t row;
        index_t col;
    };

    __device__ BlockwiseBatchGemmBlockABlockBThreadCTransANormalBNormalC_V2()
    {
        static_assert(BatchSize % BatchPerThread == 0,
                      "wrong! BatchSize is not dividable by BatchPerThread");

        constexpr index_t BatchThreadWork = BatchSize / BatchPerThread;

        constexpr index_t ThreadPerLevel1Cluster =
            MLevel0Cluster * NLevel0Cluster * MLevel1Cluster * NLevel1Cluster;

        static_assert(BlockSize == BatchThreadWork * ThreadPerLevel1Cluster,
                      "wrong! wrong blocksize\n");

        constexpr auto a_block_mtx  = BlockMatrixA{};
        constexpr auto b_block_mtx  = BlockMatrixB{};
        constexpr auto c_thread_mtx = ThreadMatrixC{};

        static_assert(a_block_mtx.NRow() == b_block_mtx.NRow(),
                      "wrong! K dimension not consistent\n");

        constexpr index_t M = a_block_mtx.NCol(); // A is transposed
        constexpr index_t N = b_block_mtx.NCol();
        constexpr index_t K = a_block_mtx.NRow();

        constexpr index_t MPerThread = c_thread_mtx.NRow();
        constexpr index_t NPerThread = c_thread_mtx.NCol();

        static_assert((MPerThread % MPerThreadSubC == 0) && (NPerThread % NPerThreadSubC == 0),
                      "wrong! Cannot evenly divide thread work among repeat \n");

        constexpr index_t MRepeat = MPerThread / MPerThreadSubC;
        constexpr index_t NRepeat = NPerThread / NPerThreadSubC;

        static_assert((M % MRepeat == 0) && (N % NRepeat == 0),
                      "wrong! Cannot evenly divide work among repeat\n");

        constexpr index_t MPerLevel1Cluster = M / MRepeat;
        constexpr index_t NPerLevel1Cluster = N / NRepeat;

        static_assert((MPerLevel1Cluster % MLevel1Cluster == 0) &&
                          (NPerLevel1Cluster % NLevel1Cluster == 0),
                      "wrong! Cannot evenly divide work among Level1Cluster\n");

        constexpr index_t MPerLevel0Cluster = MPerLevel1Cluster / MLevel1Cluster;
        constexpr index_t NPerLevel0Cluster = NPerLevel1Cluster / NLevel1Cluster;

        static_assert((MPerLevel0Cluster % MLevel0Cluster == 0) &&
                          (NPerLevel0Cluster % NLevel0Cluster == 0),
                      "wrong! Cannot evenly divide work among Level0Cluster\n");

        static_assert((MPerThreadSubC == MPerLevel0Cluster / MLevel0Cluster) &&
                          (NPerThreadSubC == NPerLevel0Cluster / NLevel0Cluster),
                      "wrong! thread work size is wrong\n");

        const auto c_thread_mtx_index = GetBeginOfThreadMatrixC(get_thread_local_1d_id());

        mMyThreadOffsetA = c_thread_mtx_index.batch * BlockMatrixStrideA +
                           a_block_mtx.Get1dIndex(0, c_thread_mtx_index.row);

        mMyThreadOffsetB = c_thread_mtx_index.batch * BlockMatrixStrideB +
                           b_block_mtx.Get1dIndex(0, c_thread_mtx_index.col);

#if 0
        if(get_thread_local_1d_id() == 0 && get_block_1d_id() == 0)
        {
            print_ConstantMatrixDescriptor(BlockMatrixA{}, "a_block_mtx: ");
            print_ConstantMatrixDescriptor(BlockMatrixB{}, "b_block_mtx: ");
            print_ConstantMatrixDescriptor(ThreadMatrixC{}, "c_thread_mtx: ");

            printf("%u %u, %u %u %u, %u %u\n",
                   get_block_1d_id(),
                   get_thread_local_1d_id(),
                   c_thread_mtx_index.batch,
                   c_thread_mtx_index.row,
                   c_thread_mtx_index.col,
                   mMyThreadOffsetA,
                   mMyThreadOffsetB);
        }
#endif
    }

    __device__ MatrixIndex GetBeginOfThreadMatrixC(index_t thread_id) const
    {
        constexpr index_t BatchThreadWork = BatchSize / BatchPerThread;

        constexpr index_t ThreadPerLevel1Cluster =
            MLevel0Cluster * NLevel0Cluster * MLevel1Cluster * NLevel1Cluster;

        constexpr index_t ThreadPerLevel0Cluster = MLevel0Cluster * NLevel0Cluster;

        index_t batch_work_id = thread_id / ThreadPerLevel1Cluster;
        index_t cluster_id    = thread_id - batch_work_id * ThreadPerLevel1Cluster;

        index_t level1_id   = cluster_id / ThreadPerLevel0Cluster;
        index_t level1_m_id = level1_id / NLevel1Cluster;
        index_t level1_n_id = level1_id % NLevel1Cluster;

        index_t level0_id   = cluster_id % ThreadPerLevel0Cluster;
        index_t level0_m_id = level0_id / NLevel0Cluster;
        index_t level0_n_id = level0_id % NLevel0Cluster;

        constexpr index_t MPerLevel0Cluster = MPerThreadSubC * MLevel0Cluster;
        constexpr index_t NPerLevel0Cluster = NPerThreadSubC * NLevel0Cluster;

        return MatrixIndex{batch_work_id * BatchPerThread,
                           level1_m_id * MPerLevel0Cluster + level0_m_id * MPerThreadSubC,
                           level1_n_id * NPerLevel0Cluster + level0_n_id * NPerThreadSubC};
    }

    // this should be optimized away if input is known
    __device__ static MatrixIndex
    GetDistanceFromBeginOfThreadMatrixC(index_t batch_in_c, index_t m_in_c, index_t n_in_c)
    {
        constexpr auto c_thread_mtx = ThreadMatrixC{};

        constexpr index_t MPerThread = c_thread_mtx.NRow();
        constexpr index_t NPerThread = c_thread_mtx.NCol();

        constexpr index_t MRepeat = MPerThread / MPerThreadSubC;
        constexpr index_t NRepeat = NPerThread / NPerThreadSubC;

        constexpr index_t MPerLevel1Cluster = MPerThreadSubC * MLevel0Cluster * MLevel1Cluster;
        constexpr index_t NPerLevel1Cluster = NPerThreadSubC * NLevel0Cluster * NLevel1Cluster;

        index_t m_repeat = m_in_c / MPerThreadSubC;
        index_t n_repeat = n_in_c / NPerThreadSubC;

        index_t m_in_sub_c = m_in_c % MPerThreadSubC;
        index_t n_in_sub_c = n_in_c % NPerThreadSubC;

        return MatrixIndex{batch_in_c,
                           m_repeat * MPerLevel1Cluster + m_in_sub_c,
                           n_repeat * NPerLevel1Cluster + n_in_sub_c};
    }

    template <class FloatA, class FloatB, class FloatC, class Accumulator>
    __device__ void Run(const FloatA* __restrict__ p_a_block,
                        const FloatB* __restrict__ p_b_block,
                        FloatC* __restrict__ p_c_thread,
                        Accumulator f_accum) const
    {
        constexpr auto True  = integral_constant<bool, true>{};
        constexpr auto False = integral_constant<bool, false>{};

        constexpr auto a_block_mtx  = BlockMatrixA{};
        constexpr auto b_block_mtx  = BlockMatrixB{};
        constexpr auto c_thread_mtx = ThreadMatrixC{};

        constexpr index_t KPerBlock = a_block_mtx.NRow(); // A is transposed

        constexpr index_t MPerThread = c_thread_mtx.NRow();
        constexpr index_t NPerThread = c_thread_mtx.NCol();

        // thread A, B for GEMM
        //   A is transposed, b is not
        constexpr auto a_thread_mtx =
            make_ConstantMatrixDescriptor(Number<KPerThreadLoop>{}, Number<MPerThread>{});

        constexpr auto b_thread_mtx =
            make_ConstantMatrixDescriptor(Number<KPerThreadLoop>{}, Number<NPerThread>{});

        // thread A-sub, B-sub for copy
        constexpr auto a_thread_sub_mtx = make_ConstantMatrixDescriptor(
            Number<KPerThreadLoop>{}, Number<MPerThreadSubC>{}, Number<MPerThread>{});

        constexpr auto b_thread_sub_mtx = make_ConstantMatrixDescriptor(
            Number<KPerThreadLoop>{}, Number<NPerThreadSubC>{}, Number<NPerThread>{});

        FloatA p_a_thread[a_thread_mtx.GetElementSpace()];
        FloatB p_b_thread[b_thread_mtx.GetElementSpace()];

        constexpr index_t MPerLevel1Cluster = MPerThreadSubC * MLevel0Cluster * MLevel1Cluster;
        constexpr index_t NPerLevel1Cluster = NPerThreadSubC * NLevel0Cluster * NLevel1Cluster;

        constexpr index_t MRepeat = MPerThread / MPerThreadSubC;
        constexpr index_t NRepeat = NPerThread / NPerThreadSubC;

// loop over k
#pragma unroll
        for(index_t k_begin = 0; k_begin < KPerBlock; k_begin += KPerThreadLoop)
        {
// read first batch of A, B
//   copy A-sub to form A
#pragma unroll
            for(index_t m_repeat = 0; m_repeat < MRepeat; ++m_repeat)
            {
                threadwise_matrix_copy(
                    a_block_mtx,
                    p_a_block + a_block_mtx.Get1dIndex(k_begin, m_repeat * MPerLevel1Cluster) +
                        mMyThreadOffsetA,
                    a_thread_mtx,
                    p_a_thread + a_thread_mtx.Get1dIndex(0, m_repeat * MPerThreadSubC),
                    a_thread_sub_mtx.GetLengths());
            }

//   copy B-sub to form B
#pragma unroll
            for(index_t n_repeat = 0; n_repeat < NRepeat; ++n_repeat)
            {
                threadwise_matrix_copy(
                    b_block_mtx,
                    p_b_block + b_block_mtx.Get1dIndex(k_begin, n_repeat * NPerLevel1Cluster) +
                        mMyThreadOffsetB,
                    b_thread_mtx,
                    p_b_thread + b_thread_mtx.Get1dIndex(0, n_repeat * NPerThreadSubC),
                    b_thread_sub_mtx.GetLengths());
            }

// loop over batch
#pragma unroll
            for(index_t ib = 0; ib + 1 < BatchPerThread; ++ib)
            {
                // do current batch of gemm
                threadwise_gemm(a_thread_mtx,
                                True,
                                p_a_thread,
                                b_thread_mtx,
                                False,
                                p_b_thread,
                                c_thread_mtx,
                                False,
                                p_c_thread + ib * ThreadMatrixStrideC,
                                f_accum);

                // read next batch of a, b
                if(BlockMatrixStrideA != 0)
                {
#pragma unroll
                    for(index_t m_repeat = 0; m_repeat < MRepeat; ++m_repeat)
                    {
                        threadwise_matrix_copy(
                            a_block_mtx,
                            p_a_block +
                                a_block_mtx.Get1dIndex(k_begin, m_repeat * MPerLevel1Cluster) +
                                (ib + 1) * BlockMatrixStrideA + mMyThreadOffsetA,
                            a_thread_mtx,
                            p_a_thread + a_thread_mtx.Get1dIndex(0, m_repeat * MPerThreadSubC),
                            a_thread_sub_mtx.GetLengths());
                    }
                }

                if(BlockMatrixStrideB != 0)
                {
#pragma unroll
                    for(index_t n_repeat = 0; n_repeat < NRepeat; ++n_repeat)
                    {
                        threadwise_matrix_copy(
                            b_block_mtx,
                            p_b_block +
                                b_block_mtx.Get1dIndex(k_begin, n_repeat * NPerLevel1Cluster) +
                                (ib + 1) * BlockMatrixStrideB + mMyThreadOffsetB,
                            b_thread_mtx,
                            p_b_thread + b_thread_mtx.Get1dIndex(0, n_repeat * NPerThreadSubC),
                            b_thread_sub_mtx.GetLengths());
                    }
                }
            }

            // do last batch of gemm
            threadwise_gemm(a_thread_mtx,
                            True,
                            p_a_thread,
                            b_thread_mtx,
                            False,
                            p_b_thread,
                            c_thread_mtx,
                            False,
                            p_c_thread + (BatchPerThread - 1) * ThreadMatrixStrideC,
                            f_accum);
        }
    }

    template <class FloatA, class FloatB, class FloatC, class Accumulator>
    __device__ void Run_v3(const FloatA* __restrict__ p_a_block,
                           const FloatB* __restrict__ p_b_block,
                           FloatC* __restrict__ p_c_thread,
                           Accumulator f_accum) const
    {
        constexpr auto True  = integral_constant<bool, true>{};
        constexpr auto False = integral_constant<bool, false>{};

        constexpr auto a_block_mtx  = BlockMatrixA{};
        constexpr auto b_block_mtx  = BlockMatrixB{};
        constexpr auto c_thread_mtx = ThreadMatrixC{};

        constexpr index_t KPerBlock = a_block_mtx.NRow(); // A is transposed

        constexpr index_t MPerThread = c_thread_mtx.NRow();
        constexpr index_t NPerThread = c_thread_mtx.NCol();

        // thread A, B for GEMM
        //   A is transposed, b is not
        constexpr auto a_thread_mtx =
            make_ConstantMatrixDescriptor(Number<KPerThreadLoop>{}, Number<MPerThread>{});

        constexpr auto b_thread_mtx =
            make_ConstantMatrixDescriptor(Number<KPerThreadLoop>{}, Number<NPerThread>{});

        // thread A-sub, B-sub for copy
        constexpr auto a_thread_sub_mtx = make_ConstantMatrixDescriptor(
            Number<KPerThreadLoop>{}, Number<MPerThreadSubC>{}, Number<MPerThread>{});

        constexpr auto b_thread_sub_mtx = make_ConstantMatrixDescriptor(
            Number<KPerThreadLoop>{}, Number<NPerThreadSubC>{}, Number<NPerThread>{});

        FloatA p_a_thread[a_thread_mtx.GetElementSpace()];
        FloatB p_b_thread[b_thread_mtx.GetElementSpace()];

        constexpr index_t MPerLevel1Cluster = MPerThreadSubC * MLevel0Cluster * MLevel1Cluster;
        constexpr index_t NPerLevel1Cluster = NPerThreadSubC * NLevel0Cluster * NLevel1Cluster;

        constexpr index_t MRepeat = MPerThread / MPerThreadSubC;
        constexpr index_t NRepeat = NPerThread / NPerThreadSubC;

        // loop over k
        //#pragma unroll
        for(index_t k_begin = 0; k_begin < KPerBlock; k_begin += KPerThreadLoop)
        {
            // read first batch of A, B
            //   copy A-sub to form A
            //#pragma unroll
            for(index_t m_repeat = 0; m_repeat < MRepeat; ++m_repeat)
            {
                for(index_t i = 0; i < a_thread_sub_mtx.NRow(); ++i)
                {
#if 1
                    for(index_t j = 0; j < a_thread_sub_mtx.NCol(); ++j)
                    {
                        p_a_thread[a_thread_mtx.Get1dIndex(i, m_repeat * MPerThreadSubC + j)] =
                            p_a_block[a_block_mtx.Get1dIndex(k_begin + i,
                                                             m_repeat * MPerLevel1Cluster + j) +
                                      mMyThreadOffsetA];
                    }
#else
                    static_assert(a_thread_sub_mtx.NCol() == 4, "asm only read 4xfp32");

#endif
                }
            }

            //   copy B-sub to form B
            //#pragma unroll
            for(index_t n_repeat = 0; n_repeat < NRepeat; ++n_repeat)
            {
                for(index_t i = 0; i < b_thread_sub_mtx.NRow(); ++i)
                {
                    for(index_t j = 0; j < b_thread_sub_mtx.NCol(); ++j)
                    {
                        p_b_thread[b_thread_mtx.Get1dIndex(i, n_repeat * NPerThreadSubC + j)] =
                            p_b_block[b_block_mtx.Get1dIndex(k_begin + i,
                                                             n_repeat * MPerLevel1Cluster + j) +
                                      mMyThreadOffsetB];
                    }
                }
            }

            // loop over batch
            //#pragma unroll
            for(index_t ib = 0; ib + 1 < BatchPerThread; ++ib)
            {
                // do current batch of gemm
                for(index_t k = 0; k < a_thread_mtx.NRow(); ++k)
                {
#if 0
                    for(index_t i = 0; i < c_thread_mtx.NRow(); ++i)
                    {
                        for(index_t j = 0; j < c_thread_mtx.NCol(); ++j)
                        {
                            const index_t aindex =
                                a_thread_mtx.Get1dIndex(k, i); // A is transposed
                            const index_t bindex = b_thread_mtx.Get1dIndex(k, j);
                            const index_t cindex =
                                c_thread_mtx.Get1dIndex(i, j) + ib * ThreadMatrixStrideC;

                            f_accum(p_c_thread[cindex], p_a_thread[aindex] * p_b_thread[bindex]);
                        }
                    }
#elif 1
                    static_assert(c_thread_mtx.NRow() == 16 && c_thread_mtx.NCol() == 4,
                                  "asm is only for 16x4");

                    const index_t bindex = b_thread_mtx.Get1dIndex(k, 0);
                    for(index_t i = 0; i < c_thread_mtx.NRow(); ++i)
                    {
                        const index_t aindex = a_thread_mtx.Get1dIndex(k, i); // A is transposed
                        const index_t cindex = c_thread_mtx.Get1dIndex(i, 0);

                        asm volatile("\n \
                            v_mac_f32 %0, %4, %5 \n \
                            v_mac_f32 %1, %4, %6 \n \
                            v_mac_f32 %2, %4, %7 \n \
                            v_mac_f32 %3, %4, %8 \n \
                            "
                                     : "=v"(p_c_thread[cindex + 0]),
                                       "=v"(p_c_thread[cindex + 1]),
                                       "=v"(p_c_thread[cindex + 2]),
                                       "=v"(p_c_thread[cindex + 3])
                                     : "v"(p_a_thread[aindex]),
                                       "v"(p_b_thread[bindex + 0]),
                                       "v"(p_b_thread[bindex + 1]),
                                       "v"(p_b_thread[bindex + 2]),
                                       "v"(p_b_thread[bindex + 3]),
                                       "0"(p_c_thread[cindex + 0]),
                                       "1"(p_c_thread[cindex + 1]),
                                       "2"(p_c_thread[cindex + 2]),
                                       "3"(p_c_thread[cindex + 3]));
                    }
#endif
                }

                // read next batch of a, b
                if(BlockMatrixStrideA != 0)
                {
                    //#pragma unroll
                    for(index_t m_repeat = 0; m_repeat < MRepeat; ++m_repeat)
                    {
                        for(index_t i = 0; i < a_thread_sub_mtx.NRow(); ++i)
                        {
                            for(index_t j = 0; j < a_thread_sub_mtx.NCol(); ++j)
                            {
                                p_a_thread[a_thread_mtx.Get1dIndex(i,
                                                                   m_repeat * MPerThreadSubC + j)] =
                                    p_a_block[a_block_mtx.Get1dIndex(
                                                  k_begin + i, m_repeat * MPerLevel1Cluster + j) +
                                              (ib + 1) * BlockMatrixStrideA + mMyThreadOffsetA];
                            }
                        }
                    }
                }

                if(BlockMatrixStrideB != 0)
                {
                    //#pragma unroll
                    for(index_t n_repeat = 0; n_repeat < NRepeat; ++n_repeat)
                    {
                        for(index_t i = 0; i < b_thread_sub_mtx.NRow(); ++i)
                        {
                            for(index_t j = 0; j < b_thread_sub_mtx.NCol(); ++j)
                            {
                                p_b_thread[b_thread_mtx.Get1dIndex(i,
                                                                   n_repeat * NPerThreadSubC + j)] =
                                    p_b_block[b_block_mtx.Get1dIndex(
                                                  k_begin + i, n_repeat * MPerLevel1Cluster + j) +
                                              (ib + 1) * BlockMatrixStrideB + mMyThreadOffsetB];
                            }
                        }
                    }
                }
            }

            // do last batch of gemm
            for(index_t k = 0; k < a_thread_mtx.NRow(); ++k)
            {
#if 0
                for(index_t i = 0; i < c_thread_mtx.NRow(); ++i)
                {
                    for(index_t j = 0; j < c_thread_mtx.NCol(); ++j)
                    {
                        const index_t aindex = a_thread_mtx.Get1dIndex(k, i); // A is transposed
                        const index_t bindex = b_thread_mtx.Get1dIndex(k, j);
                        const index_t cindex = c_thread_mtx.Get1dIndex(i, j) +
                                                (BatchPerThread - 1) * ThreadMatrixStrideC;

                        f_accum(p_c_thread[cindex], p_a_thread[aindex] * p_b_thread[bindex]);
                    }
                }
#elif 1
                static_assert(c_thread_mtx.NRow() == 16 && c_thread_mtx.NCol() == 4,
                              "asm is only for 16x4");

                const index_t bindex = b_thread_mtx.Get1dIndex(k, 0);
                for(index_t i = 0; i < c_thread_mtx.NRow(); ++i)
                {
                    const index_t aindex = a_thread_mtx.Get1dIndex(k, i); // A is transposed
                    const index_t cindex =
                        c_thread_mtx.Get1dIndex(i, 0) + (BatchPerThread - 1) * ThreadMatrixStrideC;

                    asm volatile("\n \
                            v_mac_f32 %0, %4, %5 \n \
                            v_mac_f32 %1, %4, %6 \n \
                            v_mac_f32 %2, %4, %7 \n \
                            v_mac_f32 %3, %4, %8 \n \
                            "
                                 : "=v"(p_c_thread[cindex + 0]),
                                   "=v"(p_c_thread[cindex + 1]),
                                   "=v"(p_c_thread[cindex + 2]),
                                   "=v"(p_c_thread[cindex + 3])
                                 : "v"(p_a_thread[aindex]),
                                   "v"(p_b_thread[bindex + 0]),
                                   "v"(p_b_thread[bindex + 1]),
                                   "v"(p_b_thread[bindex + 2]),
                                   "v"(p_b_thread[bindex + 3]),
                                   "0"(p_c_thread[cindex + 0]),
                                   "1"(p_c_thread[cindex + 1]),
                                   "2"(p_c_thread[cindex + 2]),
                                   "3"(p_c_thread[cindex + 3]));
                }
#endif
            }
        }
    }

    template <class BlockMatrixC, index_t BlockMatrixStrideC, class FloatC>
    __device__ void CopyThreadMatrixCToBlockMatrixC(const FloatC* __restrict__ p_c_thread,
                                                    FloatC* __restrict__ p_c_block) const
    {
        constexpr auto c_block_mtx  = BlockMatrixC{};
        constexpr auto c_thread_mtx = ThreadMatrixC{};

        constexpr index_t MPerThread = c_thread_mtx.NRow();
        constexpr index_t NPerThread = c_thread_mtx.NCol();

        constexpr auto c_thread_sub_mtx = make_ConstantMatrixDescriptor(
            Number<MPerThreadSubC>{}, Number<NPerThreadSubC>{}, Number<NPerThread>{});

        constexpr index_t MPerLevel1Cluster = MPerThreadSubC * MLevel0Cluster * MLevel1Cluster;
        constexpr index_t NPerLevel1Cluster = NPerThreadSubC * NLevel0Cluster * NLevel1Cluster;

        constexpr index_t MRepeat = MPerThread / MPerThreadSubC;
        constexpr index_t NRepeat = NPerThread / NPerThreadSubC;

        const auto c_thread_mtx_begin = GetBeginOfThreadMatrixC(get_thread_local_1d_id());

        const index_t c_thread_offset =
            c_thread_mtx_begin.batch * BlockMatrixStrideC +
            c_block_mtx.Get1dIndex(c_thread_mtx_begin.row, c_thread_mtx_begin.col);

        for(index_t m_repeat = 0; m_repeat < MRepeat; ++m_repeat)
        {
            for(index_t n_repeat = 0; n_repeat < NRepeat; ++n_repeat)
            {
                threadwise_matrix_copy(
                    c_thread_sub_mtx,
                    p_c_thread +
                        c_thread_sub_mtx.Get1dIndex(m_repeat * MPerLevel1Cluster,
                                                    n_repeat * NPerLevel1Cluster),
                    c_block_mtx,
                    p_c_block +
                        c_block_mtx.Get1dIndex(m_repeat * MPerLevel1Cluster,
                                               n_repeat * NPerLevel1Cluster) +
                        c_thread_offset,
                    c_thread_sub_mtx.GetLengths());
            }
        }
    }
};
