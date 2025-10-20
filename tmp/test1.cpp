#include <bits/stdc++.h>
using namespace std;

/*
有3种硬币若干个,面值分别是1分、2分、5分,如果要凑够1毛5,设计一个算法求有哪些组合方式,共多少种组合方式。
*/
void solve1()
{
    int n1 = 0;
    for (int i = 0; i <= 15; i++)
    {
        for (int j = 0; j <= 7; j++)
        {
            for (int k = 0; k <= 3; k++)
            {
                if (i + 2 * j + 5 * k == 15)
                {
                    n1++;
                }
            }
        }
    }
    cout << n1 << "\n";
}
// 创建两个向量数组arr1和arr2，计算这两个向量数组对应位置元素的相乘的和，即向量arr1和arr2的内积，在参考代码中合适的位置添加计数变量count，用于记录关键代码执行的次数
void solve2(vector<int> arr1, vector<int> arr2)
{
    if (arr1.size() != arr2.size())
    {
        throw invalid_argument("arr1 and arr2 must have the same size");
    }
    int count = 0;
    int n = arr1.size();
    int result = 0;
    for (int i = 0; i < n; i++)
    {
        count++;
        result += arr1[i] * arr2[i];
    }
    cout << "result:" << result << "\n";
    cout << "count:" << count << "\n";
}

// 有一个整数序列是0,5,6,12,19,32,52,…,其中第1项为0,第2项为5,第3项6,以此类推,采用迭代算法和递归算法求该数列的第n(其中n>1)项。（提示：f(1)=0;f(2)=5;f(3)=6=f(1)+f(2)+1;f(4)=f(2)+f(3)+1。
void solve3(int n)
{
    if (n == 1)
    {
        cout << 0 << "\n";
    }
    else if (n == 2)
    {
        cout << 5 << "\n";
    }
    else
    {
        int a = 0, b = 5;
        for (int i = 3; i <= n; i++)
        {
            int c = a + b + 1;
            a = b;
            b = c;
        }
        cout << b << "\n";
    }
}
int solve3_1(int n)
{
    if (n == 1)
    {
        return 0;
    }
    else if (n == 2)
    {
        return 5;
    }
    else
    {
        return solve3_1(n - 1) + solve3_1(n - 2) + 1;
    }
}

// 使用蛮力算法求解0-1背包问题，即：已知像向量数组W为物品的体积，V为物品的价值，W[0]表示物品0的体积，V[0]表示物品0的价值，背包的容积为M，物品数量为n，如何装物品使得不超过背包容积的情况下，背包所装物品的价值最大。
void solve4()
{
    
}

int main(void)
{
    cout.sync_with_stdio(0), cin.tie(0), cout.tie(0);
    solve3(4);
    cout << solve3_1(4);
}