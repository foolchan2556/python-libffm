#include <iostream>
#include <iomanip>
#include <fstream>
#include <stdexcept>
#include <algorithm>
#include <new>
#include <memory>
#include <cmath>
#include <vector>
#include <pmmintrin.h>

#if defined USEOMP
#include <omp.h>
#endif

#include "ffm.h"

#include <graphlab/logger/logger.hpp>
#include <graphlab/timer/timer.hpp>
#include <graphlab/flexible_type/flexible_type.hpp>


namespace ffm {

size_t get_column_index(graphlab::gl_sframe sf, std::string colname) {
  const auto colnames = sf.column_names();
  for (size_t i = 0; i < colnames.size(); ++i) {
    if (colnames[i] == colname) {
      return i;
    }
  }
  return -1;
}


namespace {

using namespace std;
using namespace graphlab;


ffm_int const kALIGNByte = 16;
ffm_int const kALIGN = kALIGNByte/sizeof(ffm_float);

inline ffm_float wTx(
    ffm_node *begin,
    ffm_node *end,
    ffm_float r,
    ffm_model &model, 
    ffm_float kappa=0, 
    ffm_float eta=0, 
    ffm_float lambda=0, 
    bool do_update=false)
{
    ffm_long align0 = (ffm_long)model.k*2;
    ffm_long align1 = (ffm_long)model.m*align0;

    __m128 XMMkappa = _mm_set1_ps(kappa);
    __m128 XMMeta = _mm_set1_ps(eta);
    __m128 XMMlambda = _mm_set1_ps(lambda);

    __m128 XMMt = _mm_setzero_ps();

    for(ffm_node *N1 = begin; N1 != end; N1++)
    {
        ffm_int j1 = N1->j;
        ffm_int f1 = N1->f;
        ffm_float v1 = N1->v;
        if(j1 >= model.n || f1 >= model.m)
            continue;

        for(ffm_node *N2 = N1+1; N2 != end; N2++)
        {
            ffm_int j2 = N2->j;
            ffm_int f2 = N2->f;
            ffm_float v2 = N2->v;
            if(j2 >= model.n || f2 >= model.m || f1 == f2)
                continue;

            ffm_float *w1 = model.W + j1*align1 + f2*align0;
            ffm_float *w2 = model.W + j2*align1 + f1*align0;

            __m128 XMMv = _mm_set1_ps(2.0f*v1*v2*r);

            if(do_update)
            {
                __m128 XMMkappav = _mm_mul_ps(XMMkappa, XMMv);

                ffm_float *wg1 = w1 + model.k;
                ffm_float *wg2 = w2 + model.k;
                for(ffm_int d = 0; d < model.k; d += 4)
                {
                    __m128 XMMw1 = _mm_load_ps(w1+d);
                    __m128 XMMw2 = _mm_load_ps(w2+d);

                    __m128 XMMwg1 = _mm_load_ps(wg1+d);
                    __m128 XMMwg2 = _mm_load_ps(wg2+d);

                    __m128 XMMg1 = _mm_add_ps(
                                   _mm_mul_ps(XMMlambda, XMMw1),
                                   _mm_mul_ps(XMMkappav, XMMw2));
                    __m128 XMMg2 = _mm_add_ps(
                                   _mm_mul_ps(XMMlambda, XMMw2),
                                   _mm_mul_ps(XMMkappav, XMMw1));

                    XMMwg1 = _mm_add_ps(XMMwg1, _mm_mul_ps(XMMg1, XMMg1));
                    XMMwg2 = _mm_add_ps(XMMwg2, _mm_mul_ps(XMMg2, XMMg2));

                    XMMw1 = _mm_sub_ps(XMMw1, _mm_mul_ps(XMMeta, 
                            _mm_mul_ps(_mm_rsqrt_ps(XMMwg1), XMMg1)));
                    XMMw2 = _mm_sub_ps(XMMw2, _mm_mul_ps(XMMeta, 
                            _mm_mul_ps(_mm_rsqrt_ps(XMMwg2), XMMg2)));

                    _mm_store_ps(w1+d, XMMw1);
                    _mm_store_ps(w2+d, XMMw2);

                    _mm_store_ps(wg1+d, XMMwg1);
                    _mm_store_ps(wg2+d, XMMwg2);
                }
            }
            else
            {
                for(ffm_int d = 0; d < model.k; d += 4)
                {
                    __m128  XMMw1 = _mm_load_ps(w1+d);
                    __m128  XMMw2 = _mm_load_ps(w2+d);

                    XMMt = _mm_add_ps(XMMt, 
                           _mm_mul_ps(_mm_mul_ps(XMMw1, XMMw2), XMMv));
                }
            }
        }
    }

    if(do_update)
        return 0;

    XMMt = _mm_hadd_ps(XMMt, XMMt);
    XMMt = _mm_hadd_ps(XMMt, XMMt);
    ffm_float t;
    _mm_store_ss(&t, XMMt);

    return t;
}

ffm_float* malloc_aligned_float(ffm_long size)
{
    void *ptr;

    int status = posix_memalign(&ptr, kALIGNByte, size*sizeof(ffm_float));

    if(status != 0)
        throw bad_alloc();
    
    return (ffm_float*)ptr;
}

ffm_model* init_model(ffm_int n, ffm_int m, ffm_parameter param)
{
    ffm_int k_aligned = (ffm_int)ceil((ffm_double)param.k/kALIGN)*kALIGN;

    ffm_model *model = new ffm_model;
    model->n = n;
    model->k = k_aligned;
    model->m = m;
    model->W = nullptr;
    model->normalization = param.normalization;
    
    try
    {
        model->W = malloc_aligned_float((ffm_long)n*m*k_aligned*2);
    }
    catch(bad_alloc const &e)
    {
        ffm_destroy_model(&model);
        throw;
    }

    ffm_float coef = 0.5/sqrt(param.k);
    ffm_float *w = model->W;

    for(ffm_int j = 0; j < model->n; j++)
    {
        for(ffm_int f = 0; f < model->m; f++)
        {
            for(ffm_int d = 0; d < param.k; d++, w++)
                *w = coef*drand48();
            for(ffm_int d = param.k; d < k_aligned; d++, w++)
                *w = 0;
            for(ffm_int d = k_aligned; d < 2*k_aligned; d++, w++)
                *w = 1;
        }
    }

    return model;
}

void shrink_model(ffm_model &model, ffm_int k_new)
{
    for(ffm_int j = 0; j < model.n; j++)
    {
        for(ffm_int f = 0; f < model.m; f++)
        {
            ffm_float *src = model.W + (j*model.m+f)*model.k*2;
            ffm_float *dst = model.W + (j*model.m+f)*k_new;
            copy(src, src+k_new, dst);
        }
    }

    model.k = k_new;
}

shared_ptr<ffm_model> train(
    ffm_problem *tr, 
    vector<ffm_int> &order, 
    ffm_parameter param, 
    ffm_problem *va=nullptr)
{
#if defined USEOMP
    ffm_int old_nr_threads = omp_get_num_threads();
    omp_set_num_threads(param.nr_threads);
#endif

    shared_ptr<ffm_model> model = 
        shared_ptr<ffm_model>(init_model(tr->n, tr->m, param),
            [] (ffm_model *ptr) { ffm_destroy_model(&ptr); });


    if(!param.quiet)
    {
        stringstream ss;
        ss << setw(4) << "iter"
           << setw(13) << "tr_logloss";
        if(va != nullptr && va->l != 0)
        {
            ss << setw(13) << "va_logloss";
        }
        ss << endl;

        logprogress_stream << ss.str() << endl; 
    }

    size_t target_col_idx = get_column_index(tr->sf, tr->target_column); 
    // logprogress_stream << tr->target_column << " " << get_column_index(tr->sf, tr->target_column) << std::endl;
    // logprogress_stream << flex_type_enum_to_name(tr->sf.select_column(tr->target_column).dtype()) << std::endl;

    std::vector<size_t> feature_col_idxs;
    for (auto col : tr->feature_columns) { 
      // logprogress_stream << col << " " << get_column_index(tr->sf, col) << std::endl;
      // logprogress_stream << flex_type_enum_to_name(tr->sf.select_column(col).dtype()) << std::endl;

      feature_col_idxs.push_back(get_column_index(tr->sf, col));
    }

    
    for(ffm_int iter = 0; iter < param.nr_iters; iter++)
    {
      ffm_double tr_loss = 0;

      size_t i = 0;
      std::vector<ffm_node> row_nodes; 
      auto rsf = tr->sf.range_iterator();
      auto it = rsf.begin();

      for (; it != rsf.end(); ++it, ++i) { 

        row_nodes.clear();
        std::vector<flexible_type> row = *it;
        const auto& yval = row[target_col_idx];

        if (row[target_col_idx].get_type() != flex_type_enum::INTEGER) {
          log_and_throw("Response must be integer type.");
        }

        if (row[target_col_idx].get_type() != flex_type_enum::INTEGER) {
          logprogress_stream << "Column " << target_col_idx << std::endl;
          logprogress_stream << flex_type_enum_to_name(row[target_col_idx].get_type()) << std::endl;
          log_and_throw("Response must be integer type.");
        }
        ffm_float y = (yval.get<flex_int>() > 0) ? 1.0f : -1.0f;

        for (size_t col : feature_col_idxs) {
          if (row[col] != FLEX_UNDEFINED) {
            if (row[col].get_type() != flex_type_enum::DICT) {
              log_and_throw("Feature columns currently must be dict.");
            }
            const flex_dict& dv = row[col].get<flex_dict>(); 
            size_t n_values = dv.size(); 
            for(size_t k = 0; k < n_values; ++k) { 
              const std::pair<flexible_type, flexible_type>& kvp = dv[k];

              ffm_node fv;
              fv.f = col; 
              fv.j = kvp.first.get<flex_int>(); 
              fv.v = (float) kvp.second;

              row_nodes.push_back(fv);
            }
          }
        }

        ffm_node blank;
        row_nodes.push_back(blank);

        ffm_node *begin = row_nodes.data(); 

        ffm_node *end = &row_nodes.back();

        ffm_float r = 1.0;

        ffm_float t = wTx(begin, end, r, *model);
        // logprogress_stream << " y " << y 
        //                   << " #nodes " << row_nodes.size() 
        //                   <<" i " << i
        //                    << " pred " << t 
        //                   << std::endl;

        ffm_float expnyt = exp(-y*t);

        tr_loss += log(1+expnyt);

        ffm_float kappa = -y*expnyt/(1+expnyt);

        wTx(begin, end, r, *model, kappa, param.eta, param.lambda, true);

      }

      if(!param.quiet)
      {
        tr_loss /= tr->l;

        stringstream ss;
        ss << setw(4) << iter 
           << setw(13) << fixed 
           << setprecision(5) << tr_loss;
        if(va != nullptr && va->l != 0)
        {
          ffm_double va_loss = 0;

          size_t i = 0;
          std::vector<ffm_node> row_nodes; 
          auto r = va->sf.range_iterator();
          auto it = r.begin();

          for (; it != r.end(); ++it, ++i) { 

            row_nodes.clear();
            const std::vector<flexible_type>& row = *it;
            const auto& yval = row[target_col_idx];
            ffm_float y = (yval.get<flex_int>() > 0) ? 1.0f : -1.0f;

            for (size_t col : feature_col_idxs) {
              if (row[col] != FLEX_UNDEFINED) {
                if (row[col].get_type() != flex_type_enum::DICT) {
                  log_and_throw("Feature columns must be dict type.");
                }
                const flex_dict& dv = row[col].get<flex_dict>(); 
                size_t n_values = dv.size(); 
                for(size_t k = 0; k < n_values; ++k) { 
                  const std::pair<flexible_type, flexible_type>& kvp = dv[k];

                  ffm_node fv;
                  fv.f = col; 
                  fv.j = kvp.first.get<flex_int>(); 
                  fv.v = (float) kvp.second;

                  row_nodes.push_back(fv);
                }
              }
            }

            ffm_node blank;
            row_nodes.push_back(blank);

            ffm_node *begin = row_nodes.data(); 

            ffm_node *end = &row_nodes.back();

            ffm_float r = 1.0; 

            ffm_float t = wTx(begin, end, r, *model);

            ffm_float expnyt = exp(-y*t);

            va_loss += log(1+expnyt);
          }
          va_loss /= va->l;

          ss << setw(13) << fixed << setprecision(5) << va_loss;

        }
        ss << endl;
        logprogress_stream << ss.str() << endl;
      }
    }

    shrink_model(*model, param.k);

    return model;
}

} // unnamed namespace

ffm_int ffm_save_model(ffm_model *model, char const *path)
{
    ofstream f_out(path);
    if(!f_out.is_open())
        return 1;

    f_out << "n " << model->n << "\n";
    f_out << "m " << model->m << "\n";
    f_out << "k " << model->k << "\n";
    f_out << "normalization " << model->normalization << "\n";

    ffm_float *ptr = model->W;
    for(ffm_int j = 0; j < model->n; j++)
    {
        for(ffm_int f = 0; f < model->m; f++)
        {
            f_out << "w" << j << "," << f << " ";
            for(ffm_int d = 0; d < model->k; d++, ptr++)
                f_out << *ptr << " ";
            f_out << "\n";
        }
    }

    return 0;
}

ffm_model* ffm_load_model(char const *path)
{
    ifstream f_in(path);
    if(!f_in.is_open())
        return nullptr;

    string dummy;

    ffm_model *model = new ffm_model;
    model->W = nullptr;

    f_in >> dummy >> model->n >> dummy >> model->m >> dummy >> model->k 
         >> dummy >> model->normalization;

    try
    {
        model->W = malloc_aligned_float((ffm_long)model->m*model->n*model->k);
    }
    catch(bad_alloc const &e)
    {
        ffm_destroy_model(&model);
        return nullptr;
    }

    ffm_float *ptr = model->W;
    for(ffm_int j = 0; j < model->n; j++)
    {
        for(ffm_int f = 0; f < model->m; f++)
        {
            f_in >> dummy;
            for(ffm_int d = 0; d < model->k; d++, ptr++)
                f_in >> *ptr;
        }
    }

    return model;
}

void ffm_destroy_model(ffm_model **model)
{
    if(model == nullptr || *model == nullptr)
        return;
    free((*model)->W);
    delete *model;
    *model = nullptr;
}

ffm_parameter ffm_get_default_param()
{
    ffm_parameter param;

    param.eta = 0.1;
    param.lambda = 0;
    param.nr_iters = 15;
    param.k = 4;
    param.nr_threads = 1;
    param.quiet = false;
    param.normalization = false;
    param.random = true;

    return param;
}

ffm_model* train_with_validation(ffm_problem *tr, ffm_problem *va, ffm_parameter param)
{
    vector<ffm_int> order(tr->l);
    for(ffm_int i = 0; i < tr->l; i++)
        order[i] = i;

    shared_ptr<ffm_model> model = train(tr, order, param, va);

    ffm_model *model_ret = new ffm_model;

    model_ret->n = model->n;
    model_ret->m = model->m;
    model_ret->k = model->k;
    model_ret->normalization = model->normalization;

    model_ret->W = model->W;
    model->W = nullptr;

    return model_ret;
}

ffm_model* ffm_train(ffm_problem *prob, ffm_parameter param)
{
    return train_with_validation(prob, nullptr, param);
}

ffm_float ffm_predict(ffm_node *begin, ffm_node *end, ffm_model *model)
{
    ffm_float r = 1;
    if(model->normalization)
    {
        r = 0;
        for(ffm_node *N = begin; N != end; N++)
            r += N->v*N->v; 
        r = 1/sqrt(r);
    }

    ffm_long align0 = (ffm_long)model->k;
    ffm_long align1 = (ffm_long)model->m*align0;

    ffm_float t = 0;
    for(ffm_node *N1 = begin; N1 != end; N1++)
    {
        ffm_int j1 = N1->j;
        ffm_int f1 = N1->f;
        ffm_float v1 = N1->v;
        if(j1 >= model->n || f1 >= model->m)
            continue;

        for(ffm_node *N2 = N1+1; N2 != end; N2++)
        {
            ffm_int j2 = N2->j;
            ffm_int f2 = N2->f;
            ffm_float v2 = N2->v;
            if(j2 >= model->n || f2 >= model->m || f1 == f2)
                continue;

            ffm_float *w1 = model->W + j1*align1 + f2*align0;
            ffm_float *w2 = model->W + j2*align1 + f1*align0;

            ffm_float v = 2*v1*v2*r;

            for(ffm_int d = 0; d < model->k; d++)
                t += w1[d]*w2[d]*v;
        }
    }

    return 1/(1+exp(-t));
}

} // namespace ffm
