! Shared helpers for the MILA telemetry tools (telemetry_stats, make_dataset):
! reading the CSV the controllers export and formatting numbers.
module telemetry_common
  use, intrinsic :: iso_fortran_env, only: real64
  implicit none
  private

  integer, parameter, public :: dp = real64

  public :: strip_cr, split_csv, parse_real, epoch_seconds, fstr

contains

  ! Files written on Windows end their lines with CR LF; drop the CR that read() leaves behind.
  subroutine strip_cr(s)
    character(len=*), intent(inout) :: s
    integer :: l
    l = len_trim(s)
    if (l > 0) then
      if (s(l:l) == achar(13)) s(l:l) = ' '
    end if
  end subroutine strip_cr

  ! Split a comma-separated line into fields. Empty cells stay blank, and the field count is returned.
  subroutine split_csv(text, out, count)
    character(len=*), intent(in)  :: text
    character(len=*), intent(out) :: out(:)
    integer, intent(out) :: count
    integer :: p, start, l
    logical :: boundary

    out = ''
    count = 0
    start = 1
    l = len_trim(text)
    do p = 1, l + 1
      if (p > l) then
        boundary = .true.
      else
        boundary = (text(p:p) == ',')
      end if
      if (boundary) then
        count = count + 1
        if (count <= size(out) .and. p > start) out(count) = text(start:p - 1)
        start = p + 1
      end if
    end do
  end subroutine split_csv

  ! A numeric cell -> value. `good` is false for blank or unreadable cells (the robot reported nothing).
  subroutine parse_real(s, value, good)
    character(len=*), intent(in) :: s
    real(dp), intent(out) :: value
    logical, intent(out) :: good
    integer :: status
    value = 0.0_dp
    good = .false.
    if (len_trim(s) == 0) return
    read (s, *, iostat=status) value
    good = (status == 0)
  end subroutine parse_real

  ! "YYYY-MM-DD HH:MM:SS.mmm" -> seconds since 1970-01-01 (local time; only differences matter).
  function epoch_seconds(ts) result(sec)
    character(len=*), intent(in) :: ts
    real(dp) :: sec
    integer :: y, mo, d, h, mi, status, era, yoe, doy, doe
    real(dp) :: s

    sec = 0.0_dp
    read (ts, '(I4,1X,I2,1X,I2,1X,I2,1X,I2,1X,F6.3)', iostat=status) y, mo, d, h, mi, s
    if (status /= 0) return

    ! days from civil date (H. Hinnant's algorithm), valid for the Gregorian calendar
    if (mo <= 2) y = y - 1
    era = y / 400
    yoe = y - era * 400
    if (mo > 2) then
      doy = (153 * (mo - 3) + 2) / 5 + d - 1
    else
      doy = (153 * (mo + 9) + 2) / 5 + d - 1
    end if
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy
    sec = real(era * 146097 + doe - 719468, dp) * 86400.0_dp + real(h * 3600 + mi * 60, dp) + s
  end function epoch_seconds

  ! A number with d decimals and no padding, keeping the leading zero (F0.d prints ".54").
  function fstr(x, d) result(s)
    real(dp), intent(in) :: x
    integer, intent(in) :: d
    character(len=:), allocatable :: s
    character(len=32) :: buf, fmt
    write (fmt, '(A,I0,A)') '(F24.', d, ')'
    write (buf, fmt) x
    s = trim(adjustl(buf))
  end function fstr

end module telemetry_common
