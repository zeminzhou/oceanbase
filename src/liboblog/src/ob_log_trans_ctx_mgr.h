/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#ifndef OCEANBASE_LIBOBLOG_LOG_TRANS_CTX_MGR_
#define OCEANBASE_LIBOBLOG_LOG_TRANS_CTX_MGR_

#include "ob_log_trans_ctx.h"                       // TransCtx

#include "ob_easy_hazard_map.h"                     // ObEasyHazardMap
#include "storage/transaction/ob_trans_define.h"    // ObTransID

namespace oceanbase
{
namespace liboblog
{
class IObLogTransCtxMgr
{
public:
  IObLogTransCtxMgr() {}
  virtual ~IObLogTransCtxMgr() {}

public:
  /// Get the transaction context and support the creation of a new context if it does not exist
  ///
  /// @note must be called in pairs with the revert_trans_ctx() function
  ///
  /// @param [in]   key           Trans ID
  /// @param [out]  trans_ctx     returned trans context
  /// @param [in]   enable_create Whether to allow the creation of a new object when the transaction context does not exist, not allowed by default
  ///
  /// @retval OB_SUCCESS         Success
  /// @retval OB_ENTRY_NOT_EXIST tenant not exist
  /// @retval other_error_code   Fail
  virtual int get_trans_ctx(const transaction::ObTransID &key, TransCtx *&trans_ctx, bool enable_create = false) = 0;

  /// revert trans context
  ///
  /// @param trans_ctx           target trans context
  ///
  /// @retval OB_SUCCESS         Success
  /// @retval other_error_code   Fail
  virtual int revert_trans_ctx(TransCtx *trans_ctx) = 0;

  /// delete trans context
  ///
  /// @param key                 target trans context
  ///
  /// @retval OB_SUCCESS         Success
  /// @retval OB_ENTRY_NOT_EXIST trans context not exist
  /// @retval other_error_code   Fail
  virtual int remove_trans_ctx(const transaction::ObTransID &key) = 0;

  /// Updating statistical information
  virtual int update_stat_info(int trans_state) = 0;

  /// Print statistics
  virtual void print_stat_info() = 0;

  /// Get the number of transactions in a given state
  virtual int64_t get_trans_count(int trans_ctx_state) = 0;

  /// Print information on pending transactions
  virtual int dump_pending_trans_info(char *buffer, const int64_t size, int64_t &pos) = 0;

  // Do you need to sort the list of participants
  virtual bool need_sort_participant() const = 0;
};

//////////////////////////////////////////////////////////////////////////////////////////////

class ObLogTransCtxMgr : public IObLogTransCtxMgr
{
  struct Scanner
  {
    Scanner() : buffer_(NULL), buffer_size_(0), pos_(0), valid_trans_count_(0)
    {
      (void)memset(trans_count_, 0, sizeof(trans_count_));
    }

    ~Scanner() {}

    void operator() (const transaction::ObTransID &trans_id, TransCtx *trans_ctx);

    char *buffer_;
    int64_t buffer_size_;
    int64_t pos_;
    int64_t valid_trans_count_;
    int64_t trans_count_[TransCtx::TRANS_CTX_STATE_MAX];
  };

public:
  static const int64_t BLOCK_SIZE = 1 << 24;
  static const int64_t PRINT_STATE_INTERVAL = 10 * 1000 * 1000;
  typedef ObEasyHazardMap<transaction::ObTransID, TransCtx> TransCtxMap;

public:
  ObLogTransCtxMgr();
  virtual ~ObLogTransCtxMgr();

public:
  int get_trans_ctx(const transaction::ObTransID &key, TransCtx *&trans_ctx, bool enable_create = false);
  int revert_trans_ctx(TransCtx *trans_ctx);
  int remove_trans_ctx(const transaction::ObTransID &key);
  int update_stat_info(const int trans_state);
  void print_stat_info();
  int64_t get_trans_count(const int trans_ctx_state);
  int dump_pending_trans_info(char *buffer, const int64_t size, int64_t &pos);
  bool need_sort_participant() const { return need_sort_participant_; };

public:
  int init(const int64_t max_cached_trans_ctx_count, const bool need_sort_participant);
  void destroy();
  // Get the number of valid TransCtx, i.e. the number of TransCtx present in the map
  inline int64_t get_valid_trans_ctx_count() const { return map_.get_valid_count(); }

  // Get the number of allocated TransCtx objects
  inline int64_t get_alloc_trans_ctx_count() const { return map_.get_alloc_count(); }

  // Get the number of free TransCtx objects
  inline int64_t get_free_trans_ctx_count() const { return map_.get_free_count(); }

private:
  bool        inited_;
  TransCtxMap map_;
  bool        need_sort_participant_;

  /// state values
  int64_t     valid_trans_count_;                           // Current number of transactions
  int64_t     trans_count_[TransCtx::TRANS_CTX_STATE_MAX];  // Number of transactions in various states

  /// Statistical values
  int64_t     created_trans_count_;                         // Number of created transactions counted
  int64_t     last_created_trans_count_;                    // Number of transactions created at last count
  int64_t     sequenced_trans_count_;                       // Number of transactions in statistical order
  int64_t     last_sequenced_trans_count_;                  // Number of transactions in fixed order at last count
  int64_t     last_stat_time_;                              // Last statistical time

private:
  DISALLOW_COPY_AND_ASSIGN(ObLogTransCtxMgr);
};
} // namespace liboblog
} // namespace oceanbase
#endif /* OCEANBASE_LIBOBLOG_LOG_TRANS_CTX_MGR_ */
