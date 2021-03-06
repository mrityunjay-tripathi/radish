/*
 * File: span_bert_model.cc
 * Project: bert
 * Author: koth (Koth Chen)
 * -----
 * Last Modified: 2019-09-20 9:38:26
 * Modified By: koth (nobody@verycool.com)
 * -----
 * Copyright 2020 - 2019
 */
#include "radish/bert/span_bert_model.h"
#include "glog/logging.h"

namespace radish {
static Tensor batch_select(const Tensor& input, const Tensor& inds) {
  // input [B,N,D]
  // inds [B,S]  -> [B,S,D]
  // ==>output: [B,S,D]
  Tensor dummy =
      inds.unsqueeze(2).expand({inds.size(0), inds.size(1), input.size(2)});
  return input.gather(1, dummy);
}

static Tensor calc_loss_(const Tensor& pred_, const Tensor& target) {
  Tensor gold = target.contiguous().view(-1);
  Tensor pred = pred_.view({pred_.size(0) * pred_.size(1), -1});
  int n_class = pred.size(1);
  float lowp = 0.1 / static_cast<float>(n_class - 1);
  float normalizing =
      -(0.9 * log(0.9) + float(n_class - 1) * lowp * log(lowp + 1e-20));
  Tensor one_hot = torch::zeros_like(pred).scatter_(1, gold.view({-1, 1}), 1);
  one_hot.mul_(0.9 - lowp).add_(lowp);
  Tensor log_prb = torch::log_softmax(pred, 1);
  Tensor non_pad_mask = gold.ne(0);
  Tensor xent = one_hot.mul_(log_prb).neg_().sum(1);
  xent = xent.masked_select(non_pad_mask);
  return xent.sub_(normalizing).mean();
}

static Tensor calc_accuracy_(const Tensor& pred, const Tensor& target) {
  torch::NoGradGuard guard;
  Tensor predT = pred.argmax(2).contiguous().view(-1);
  Tensor gold = target.contiguous().view(-1);
  Tensor correct = predT.eq(gold);
  Tensor non_pad_mask = gold.ne(0);
  correct = correct.masked_select(non_pad_mask).sum();  // sum
  return torch::div(correct.toType(torch::kFloat32),
                    non_pad_mask.sum().toType(torch::kFloat32));
}
SpanBertOptions::SpanBertOptions(int64_t n_src_vocab, int64_t len_max_seq,
                                 int64_t d_word_vec, int64_t n_layers,
                                 int64_t n_head, int64_t d_k, int64_t d_v,
                                 int64_t d_model, int64_t d_inner,
                                 double dropout)
    : n_src_vocab_(n_src_vocab),
      len_max_seq_(len_max_seq),
      d_word_vec_(d_word_vec),
      n_layers_(n_layers),
      n_head_(n_head),
      d_k_(d_k),
      d_v_(d_v),
      d_model_(d_model),
      d_inner_(d_inner),
      dropout_(dropout) {}
SpanBertModelImpl::SpanBertModelImpl(SpanBertOptions options_)
    : options(options_) {
  encoder = TransformerEncoder(
      options.n_src_vocab(), options.len_max_seq(), options.d_word_vec(),
      options.n_layers(), options.n_head(), options.d_k(), options.d_v(),
      options.d_model(), options.d_inner(), options.dropout());
  register_module("transformer_encoder", encoder);
  proj = torch::nn::Linear(options.d_model(), options.n_src_vocab());
  register_module("final_proj", proj);
  span_hidden_proj =
      torch::nn::Linear(options.d_model() * 3, options.d_model());
  register_module("span_hidden_proj", span_hidden_proj);

  laynorm = LayerNorm(options.d_model());
  register_module("laynorm", laynorm);
  torch::NoGradGuard guard;
  proj->weight = encoder->src_word_emb->weight;
}

Tensor SpanBertModelImpl::CalcLoss(const std::vector<Tensor>& inputs,
                                   const std::vector<Tensor>& logits,
                                   std::vector<float>& evals,
                                   const Tensor& target) {
  Tensor maskedOutput = batch_select(logits[0], inputs[1]);
  Tensor spanLeftOutput = batch_select(logits[0], inputs[2]);
  Tensor spanRightOutput = batch_select(logits[0], inputs[3]);
  Tensor maskPreds = proj(maskedOutput);
  Tensor mlm_loss = calc_loss_(maskPreds, target);
  Tensor spanPos = encoder->pos_emb(inputs[1]);
  Tensor spanPreds = torch::cat({spanLeftOutput, spanRightOutput, spanPos}, 2);
  Tensor span_hidden = span_hidden_proj(spanPreds);
  span_hidden = laynorm(torch::gelu(span_hidden));
  spanPreds = proj(span_hidden);
  Tensor span_loss = calc_loss_(spanPreds, target);
  if (!is_training()) {
    float mlm_accuracy = calc_accuracy_(maskPreds, target).item().to<float>();
    evals.push_back(mlm_accuracy);
  }
  return mlm_loss.add_(span_loss);
}

/**
 *inputs- 0 -src_seq
 *        1 - masked_indexies
 *        2 - span_left
 *        3 - span_right
 *
 */
std::vector<Tensor> SpanBertModelImpl::forward(std::vector<Tensor> inputs) {
  CHECK(inputs.size() >= 4);
  // 0 - for seq
  Tensor& src_seq = inputs[0];
  auto seqLen = src_seq.size(1);
  Tensor pos_seq = torch::arange(
      0, seqLen,
      torch::TensorOptions().dtype(torch::kInt64).requires_grad(false));
  // should be same device as src seq
  pos_seq = pos_seq.repeat({src_seq.size(0), 1}).to(src_seq.device());
  auto rets = encoder(src_seq, pos_seq);
  return {rets[0]};
}

}  // namespace radish
