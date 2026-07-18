#pragma once

#include "lvgl.h"
#include "ui/widget.h"

// A deliberately minimal, throwaway widget - exists only to prove
// DashboardGrid can host multiple widgets, of varying spans, without
// knowing their concrete type, the same reasoning that justified proving
// out Navigation/the home affordance with PlaceholderScreen before real
// screens existed. Not real product UI - simulator-only. Replaced once a
// real widget exists (weather is the first, a separate follow-up pass -
// see docs/architecture/dashboard.md#widget-system).
class PlaceholderWidget : public homedeck::Widget {
public:
    PlaceholderWidget(lv_obj_t* parent, const char* text, int column_span = 1, int row_span = 1);

    lv_obj_t* Root() const override { return root_; }
    int ColumnSpan() const override { return column_span_; }
    int RowSpan() const override { return row_span_; }

private:
    lv_obj_t* root_;
    int column_span_;
    int row_span_;
};
