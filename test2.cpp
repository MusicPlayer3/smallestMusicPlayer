#include <bits/stdc++.h>
using namespace std;

int main(void)
{
    ios::sync_with_stdio(0), cin.tie(0), cout.tie(0);
    int n = 50;
    vector<int> arr(50);
    random_device rd;
    mt19937 gen(rd());
    uniform_int_distribution<> dis(0, INT_MAX);
    for (int i = 0; i < n; i++)
    {
        arr[i] = dis(gen);
    }
    sort(arr.begin(), arr.end());

    int target = arr[uniform_int_distribution<>(0, n - 1)(gen)];

    auto it = lower_bound(arr.begin(), arr.end(), target);
    if (it != arr.end() && *it == target)
    {
        cout << "Found target " << target << " at index " << distance(arr.begin(), it) << "\n";
    }
    else
    {
        cout << "Target " << target << " not found in the array.\n";
    }
    return 0;
}

/*
已知数组arr有n个整型随机数（n可自定义取值，取值范围是0到1000）组成。

1、使用C++编写排序算法，对数组按照数值进行从小到大排序（任意的排序算法），并分析该排序算法的时间复杂度；

2、给定任意一个目标数值target（可自定义取值），在排序好的数组中，设计算法找到该目标数值，并分析该查找算法的时间复杂度。
*/